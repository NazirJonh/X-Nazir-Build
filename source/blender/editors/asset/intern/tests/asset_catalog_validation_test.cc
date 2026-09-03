/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "ED_asset_catalog.hh"

#include "BLI_string.h"

#include "DNA_asset_types.h"

#include <cstring>

namespace blender::ed::asset::tests {

TEST(asset_catalog_validation, UnloadedLibraryReturnsNotYetLoaded)
{
  AssetLibraryReference ref{};
  ref.type = ASSET_LIBRARY_CUSTOM;
  STRNCPY(ref.custom_library_name, "does_not_exist_in_this_test_process");

  bUUID candidate;
  memset(&candidate, 0, sizeof(candidate));
  reinterpret_cast<uint8_t *>(&candidate)[0] = 1;

  /* A library reference nothing has ever loaded in this test process must report
   * LibraryNotYetLoaded, never Valid or NotFound outright -- this is the distinction the whole
   * async-validation design in the spec depends on. */
  EXPECT_EQ(ED_asset_catalog_validate(ref, candidate),
            AssetCatalogValidation::LibraryNotYetLoaded);
}

}  // namespace blender::ed::asset::tests
