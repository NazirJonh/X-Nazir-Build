/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Interactive sculpt-layer influence drag.
 *
 * Adds the active layer's per-corner displacement, scaled by this tick's influence delta, directly
 * into a PBVH node's position draw buffer. Keeping the data on the GPU lets an influence-drag tick
 * skip the CPU re-extract + re-upload of the whole mesh position buffer.
 *
 * The buffers are addressed as tightly-packed scalar floats (3 per vertex) to match the 12-byte
 * #position_format VBO layout, independent of std430 `float3` padding. */

#include "pbvh_infos.hh"

COMPUTE_SHADER_CREATE_INFO(pbvh_layer_drag)

void main()
{
  uint i = gl_GlobalInvocationID.x;
  if (i >= uint(verts_num)) {
    return;
  }
  uint base = i * 3u;
  position_buf[base + 0u] += delta_buf[base + 0u] * scale;
  position_buf[base + 1u] += delta_buf[base + 1u] * scale;
  position_buf[base + 2u] += delta_buf[base + 2u] * scale;
}
