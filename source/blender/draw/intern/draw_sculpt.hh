/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#pragma once

#include "BLI_enum_flags.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

struct GPUMaterial;
struct Object;
namespace gpu {
class Batch;
}

namespace draw {

struct SculptBatch {
  gpu::Batch *batch;
  int material_slot;
  int debug_index;
  float3 debug_color();
};

enum SculptBatchFeature {
  SCULPT_BATCH_DEFAULT = 0,
  SCULPT_BATCH_WIREFRAME = 1 << 0,
  SCULPT_BATCH_MASK = 1 << 1,
  SCULPT_BATCH_FACE_SET = 1 << 2,
  SCULPT_BATCH_VERTEX_COLOR = 1 << 3,
  SCULPT_BATCH_UV = 1 << 4
};
ENUM_OPERATORS(SculptBatchFeature);

/**
 * Used by engines that don't use GPUMaterials, like the Workbench and Overlay engines.
 *
 * \param per_node_multires_levels: For an adaptive Multires wireframe draw, the highest
 * subdivision level (per PBVH node) whose edges should be kept in the line index buffer. An
 * empty span keeps every edge.
 */
Vector<SculptBatch> sculpt_batches_get(const Object *ob,
                                       SculptBatchFeature features,
                                       Span<int> per_node_multires_levels = {});

/** Used by EEVEE. */
Vector<SculptBatch> sculpt_batches_per_material_get(const Object *ob,
                                                    Span<const GPUMaterial *> materials);

}  // namespace draw
}  // namespace blender
