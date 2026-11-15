/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The structural hash step every bake hash shares. Kept apart from the bake internals so a
 * generated source (a Text Decal today) can hash its own inputs without pulling the planner in.
 */

#pragma once

#include <cstdint>
#include <cstring>

namespace blender {

/** Fold \a value into the structural hash \a h. */
inline uint64_t bake_hash_mix(uint64_t h, const uint64_t value)
{
  return h * 1000003u ^ value;
}

inline uint64_t bake_hash_float(uint64_t h, const float value)
{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bake_hash_mix(h, bits);
}

}  // namespace blender
