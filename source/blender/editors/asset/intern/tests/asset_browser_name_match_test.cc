/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include "testing/testing.h"

#include "ED_asset_name_matching.hh"

#include "BKE_name_matching.hh"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "DNA_asset_types.h"
#include "DNA_listBase.h"
#include "DNA_userdef_types.h"

#include "MEM_guardedalloc.h"

namespace blender::ed::asset::tests {

/* Production passes `is_image` from filelist `filelist_asset_is_image()`:
 * `asset.get_id_type() == ID_IM` for local / external / essentials / remote Image assets. */

TEST(asset_browser_name_match, map_types_copy_is_deep)
{
  ListBaseT<AssetNameMatchIdLink> src = {nullptr, nullptr};
  AssetNameMatchIdLink *link = MEM_new<AssetNameMatchIdLink>(__func__);
  STRNCPY_UTF8(link->id, "NORMAL");
  BLI_addtail(&src, link);

  ListBaseT<AssetNameMatchIdLink> dst = {nullptr, nullptr};
  ED_asset_browser_name_match_map_types_copy(dst, src);

  ASSERT_FALSE(dst.is_empty());
  EXPECT_NE(dst.first, src.first);
  EXPECT_STREQ(static_cast<AssetNameMatchIdLink *>(dst.first)->id, "NORMAL");

  ED_asset_browser_name_match_map_types_free(src);
  EXPECT_STREQ(static_cast<AssetNameMatchIdLink *>(dst.first)->id, "NORMAL");
  ED_asset_browser_name_match_map_types_free(dst);
}

TEST(asset_browser_name_match, disabled_passes_non_image)
{
  NameMatchResolvedFilter resolved;
  EXPECT_TRUE(ED_asset_browser_name_match_entry_visible(
      false, false, false, resolved, "Brush", {}));
}

TEST(asset_browser_name_match, enabled_empty_shows_non_image_and_image)
{
  NameMatchResolvedFilter resolved;
  EXPECT_TRUE(ED_asset_browser_name_match_entry_visible(
      true, false, false, resolved, "Brush", {}));
  EXPECT_TRUE(ED_asset_browser_name_match_entry_visible(
      true, true, false, resolved, "Wood_Albedo", {}));
}

TEST(asset_browser_name_match, enabled_unresolved_hides_images_passes_non_image)
{
  NameMatchResolvedFilter resolved; /* active == false */
  EXPECT_FALSE(ED_asset_browser_name_match_entry_visible(
      true, true, true, resolved, "Wood_Normal", {}));
  EXPECT_TRUE(ED_asset_browser_name_match_entry_visible(
      true, false, true, resolved, "Brush", {}));
}

TEST(asset_browser_name_match, enabled_resolved_normal_filters_images_passes_non_image)
{
  UserDef userdef = {};
  BKE_name_matching_userdef_ensure_defaults(&userdef);
  NameMatchFilterState state;
  state.enabled = true;
  state.active_map_type_ids.add("NORMAL");
  NameMatchResolvedFilter resolved = BKE_name_match_filter_resolve(state, userdef);
  ASSERT_TRUE(resolved.active);

  EXPECT_TRUE(ED_asset_browser_name_match_entry_visible(
      true, true, true, resolved, "Wood_Normal", {}));
  EXPECT_FALSE(ED_asset_browser_name_match_entry_visible(
      true, true, true, resolved, "Wood_BaseColor", {}));
  EXPECT_TRUE(ED_asset_browser_name_match_entry_visible(
      true, false, true, resolved, "Brush", {}));

  BKE_name_matching_userdef_free(&userdef);
}

TEST(asset_browser_name_match, enabled_resolved_map_type_matches_metadata_tag)
{
  UserDef userdef = {};
  BKE_name_matching_userdef_ensure_defaults(&userdef);
  NameMatchFilterState state;
  state.enabled = true;
  state.active_map_type_ids.add("NORMAL");
  NameMatchResolvedFilter resolved = BKE_name_match_filter_resolve(state, userdef);
  ASSERT_TRUE(resolved.active);

  Vector<StringRef> matching_tags = {"map:NORMAL"};
  EXPECT_TRUE(ED_asset_browser_name_match_entry_visible(
      true, true, true, resolved, "Wood_001", matching_tags.as_span()));
  Vector<StringRef> unrelated_tags = {"map:ROUGHNESS"};
  EXPECT_FALSE(ED_asset_browser_name_match_entry_visible(
      true, true, true, resolved, "Wood_001", unrelated_tags.as_span()));

  BKE_name_matching_userdef_free(&userdef);
}

}  // namespace blender::ed::asset::tests
