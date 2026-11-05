/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The source-identity hasher shared by the PBR paint module's caches. Kept in its own header so
 * the widely included #paint_material_source.hh does not pull in <bit>/<type_traits> for it.
 */

#include <bit>
#include <cstdint>
#include <type_traits>

namespace blender::ed::sculpt_paint::material {

/** FNV-1a over the values mixed into it, the one hasher the module's source-identity keys share.
 * Floating-point values are hashed through their bits, so a key mixing floats is exact.
 * Never zero: zero means "no identity" to the callers (see #get). */
class IdentityHasher {
  uint64_t hash_ = 0xcbf29ce484222325ULL;

 public:
  template<typename T> void mix(const T &value)
  {
    uint64_t bits;
    if constexpr (std::is_floating_point_v<T>) {
      bits = std::bit_cast<uint32_t>(float(value));
    }
    else if constexpr (std::is_pointer_v<T>) {
      bits = uintptr_t(value);
    }
    else {
      bits = uint64_t(value);
    }
    hash_ = (hash_ ^ bits) * 0x100000001b3ULL;
  }

  /** Never zero: zero means "no identity" to the callers. */
  uint64_t get() const
  {
    return hash_ | 1;
  }
};

}  // namespace blender::ed::sculpt_paint::material
