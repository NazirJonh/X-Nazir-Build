/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The runtime-state storage every generated source of a layered material needs: a map keyed by
 * #ID::session_uid, the mutex that protects it, and the free/owner-transfer pair that follows a
 * material's lifetime. A source's runtime type stays its own; only the storage is shared, so the
 * next generated source does not copy the lock and owner checks again.
 */

#pragma once

#include <cstdint>
#include <mutex>

#include "BLI_map.hh"

namespace blender {

struct Material;

template<typename Runtime> class GeneratedSourceRuntime {
 public:
  Map<uint32_t, Runtime> &map()
  {
    return map_;
  }
  std::mutex &mutex()
  {
    return mutex_;
  }

  /** Drop the entry of \a ma when it owns it (the material is being freed). */
  void runtime_free(const uint32_t session_uid, Material &ma)
  {
    std::lock_guard lock(mutex_);
    const Runtime *runtime = map_.lookup_ptr(session_uid);
    /* Only the owning material may drop the entry: a same-uid old ID freed during a memfile undo
     * must leave the re-read ID's state in place. */
    if (runtime != nullptr && runtime->owner == &ma) {
      map_.remove(session_uid);
    }
  }

  /** Move the owned entry to \a dst (the material was replaced by a new ID instance). */
  void owner_transfer(const uint32_t session_uid, Material &dst, Material &src)
  {
    std::lock_guard lock(mutex_);
    Runtime *runtime = map_.lookup_ptr(session_uid);
    if (runtime != nullptr && runtime->owner == &src) {
      runtime->owner = &dst;
    }
  }

 private:
  Map<uint32_t, Runtime> map_;
  std::mutex mutex_;
};

}  // namespace blender
