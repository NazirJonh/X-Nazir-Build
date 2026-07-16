/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Utility to extend #AssetLibraryReference with C++ functionality (operators, hash function, etc).
 */

#pragma once

#include "BLI_hash.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"

#include "DNA_asset_types.h"

namespace blender {

inline bool operator==(const AssetLibraryReference &a, const AssetLibraryReference &b)
{
  if (a.type != b.type) {
    return false;
  }
  if (a.type != ASSET_LIBRARY_CUSTOM) {
    return true;
  }
  /* The name is the identity; the index is a derived cache that goes stale on a Preferences
   * reorder (see #AssetLibraryReference). Legacy references carry no name and can only be
   * compared by index. */
  if (a.custom_library_name[0] || b.custom_library_name[0]) {
    return STREQ(a.custom_library_name, b.custom_library_name);
  }
  return a.custom_library_index == b.custom_library_index;
}

template<> struct DefaultHash<AssetLibraryReference> {
  uint64_t operator()(const AssetLibraryReference &value) const
  {
    if (value.type != ASSET_LIBRARY_CUSTOM) {
      return get_default_hash(value.type);
    }
    /* Must agree with `operator==` above: hash whichever member that comparison uses. */
    if (value.custom_library_name[0]) {
      return get_default_hash(value.type, StringRef(value.custom_library_name));
    }
    return get_default_hash(value.type, value.custom_library_index);
  }
};

}  // namespace blender
