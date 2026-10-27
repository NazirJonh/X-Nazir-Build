/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include "ED_asset_shelf_brush_set_temp.hh"

#include "DNA_asset_types.h"
#include "DNA_brush_enums.h"
#include "DNA_userdef_types.h"

#include "testing/testing.h"

namespace blender::ed::asset::shelf::tests {

using brush_set_temp::Category;

/* -------------------------------------------------------------------- */
/** \name Brush type classification
 * \{ */

TEST(asset_shelf_brush_set_temp, brush_type_categories_paint_group)
{
  /* The brush types of the Paint display group (`_tools_paint_display`). */
  for (const int brush_type : {SCULPT_BRUSH_TYPE_PAINT,
                               SCULPT_BRUSH_TYPE_ERASE,
                               SCULPT_BRUSH_TYPE_SMEAR,
                               SCULPT_BRUSH_TYPE_BLUR,
                               SCULPT_BRUSH_TYPE_TEXTURE_FILL,
                               SCULPT_BRUSH_TYPE_CLONE})
  {
    EXPECT_EQ(brush_set_temp::brush_type_categories(brush_type), Category::Paint);
  }
}

TEST(asset_shelf_brush_set_temp, brush_type_categories_sculpt_group)
{
  /* Everything else belongs to the Sculpt display group. */
  for (const int brush_type : {SCULPT_BRUSH_TYPE_DRAW,
                               SCULPT_BRUSH_TYPE_SMOOTH,
                               SCULPT_BRUSH_TYPE_CLAY,
                               SCULPT_BRUSH_TYPE_CLOTH,
                               SCULPT_BRUSH_TYPE_LAYER_ERASER,
                               SCULPT_BRUSH_TYPE_CLONE_MESH})
  {
    EXPECT_EQ(brush_set_temp::brush_type_categories(brush_type), Category::Sculpt);
  }
}

TEST(asset_shelf_brush_set_temp, brush_type_categories_shared_and_unknown)
{
  /* Mask brushes work from both display groups. */
  EXPECT_EQ(brush_set_temp::brush_type_categories(SCULPT_BRUSH_TYPE_MASK),
            Category::Sculpt | Category::Paint);

  /* `eBrushSculptType` starts at 1, so 0 and -1 mean "no brush type", which decides nothing. */
  EXPECT_EQ(brush_set_temp::brush_type_categories(0), Category::Any);
  EXPECT_EQ(brush_set_temp::brush_type_categories(-1), Category::Any);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Library classification
 * \{ */

static bUserAssetLibrary make_library(const int brush_set)
{
  bUserAssetLibrary library{};
  library.type = USER_ASSET_LIBRARY_ITEM_TYPE_LEAF;
  library.brush_set = brush_set;
  return library;
}

TEST(asset_shelf_brush_set_temp, library_passes_no_filter)
{
  for (const int brush_set :
       {ASSET_LIBRARY_BRUSH_SET_NONE,
        ASSET_LIBRARY_BRUSH_SET_SCULPT,
        ASSET_LIBRARY_BRUSH_SET_PAINT,
        ASSET_LIBRARY_BRUSH_SET_BOTH})
  {
    EXPECT_TRUE(brush_set_temp::library_passes(make_library(brush_set), Category::Any));
  }
}

TEST(asset_shelf_brush_set_temp, library_passes_matrix)
{
  struct Case {
    int brush_set;
    Category required;
    bool expected;
  };
  /* A Sculpt-only library may not be offered while the shelf filters for Paint, and vice versa. */
  const Case cases[] = {
      {ASSET_LIBRARY_BRUSH_SET_NONE, Category::Sculpt, false},
      {ASSET_LIBRARY_BRUSH_SET_NONE, Category::Paint, false},
      {ASSET_LIBRARY_BRUSH_SET_SCULPT, Category::Sculpt, true},
      {ASSET_LIBRARY_BRUSH_SET_SCULPT, Category::Paint, false},
      {ASSET_LIBRARY_BRUSH_SET_PAINT, Category::Sculpt, false},
      {ASSET_LIBRARY_BRUSH_SET_PAINT, Category::Paint, true},
      {ASSET_LIBRARY_BRUSH_SET_BOTH, Category::Sculpt, true},
      {ASSET_LIBRARY_BRUSH_SET_BOTH, Category::Paint, true},
      /* A combined requirement is satisfied by either bit. */
      {ASSET_LIBRARY_BRUSH_SET_SCULPT, Category::Sculpt | Category::Paint, true},
      {ASSET_LIBRARY_BRUSH_SET_PAINT, Category::Sculpt | Category::Paint, true},
      {ASSET_LIBRARY_BRUSH_SET_NONE, Category::Sculpt | Category::Paint, false},
  };

  for (const Case &c : cases) {
    EXPECT_EQ(brush_set_temp::library_passes(make_library(c.brush_set), c.required), c.expected)
        << "brush_set=" << c.brush_set << " required=" << int(c.required);
  }
}

TEST(asset_shelf_brush_set_temp, library_passes_folder_always_passes)
{
  /* Folders are containers: the selector prunes them when their subtree has nothing to show, so
   * they must never be filtered out by their own (unused) classification. */
  bUserAssetLibrary folder{};
  folder.type = USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER;
  folder.brush_set = ASSET_LIBRARY_BRUSH_SET_NONE;

  EXPECT_TRUE(brush_set_temp::library_passes(folder, Category::Sculpt));
  EXPECT_TRUE(brush_set_temp::library_passes(folder, Category::Paint));
}

TEST(asset_shelf_brush_set_temp, essentials_passes_matrix)
{
  /* The Essentials and the Online Essentials share this predicate and its classification. */
  UserDef userdef{};

  userdef.essentials_brush_set = ASSET_LIBRARY_BRUSH_SET_NONE;
  EXPECT_TRUE(brush_set_temp::essentials_passes(userdef, Category::Any));
  EXPECT_FALSE(brush_set_temp::essentials_passes(userdef, Category::Sculpt));

  userdef.essentials_brush_set = ASSET_LIBRARY_BRUSH_SET_SCULPT;
  EXPECT_TRUE(brush_set_temp::essentials_passes(userdef, Category::Sculpt));
  EXPECT_FALSE(brush_set_temp::essentials_passes(userdef, Category::Paint));

  userdef.essentials_brush_set = ASSET_LIBRARY_BRUSH_SET_PAINT;
  EXPECT_FALSE(brush_set_temp::essentials_passes(userdef, Category::Sculpt));
  EXPECT_TRUE(brush_set_temp::essentials_passes(userdef, Category::Paint));

  userdef.essentials_brush_set = ASSET_LIBRARY_BRUSH_SET_BOTH;
  EXPECT_TRUE(brush_set_temp::essentials_passes(userdef, Category::Sculpt));
  EXPECT_TRUE(brush_set_temp::essentials_passes(userdef, Category::Paint));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tool display group
 * \{ */

TEST(asset_shelf_brush_set_temp, tool_display_category)
{
  EXPECT_EQ(brush_set_temp::tool_display_category(TOOL_DISPLAY_GROUP_NONE), Category::Any);
  EXPECT_EQ(brush_set_temp::tool_display_category(TOOL_DISPLAY_GROUP_SCULPT), Category::Sculpt);
  EXPECT_EQ(brush_set_temp::tool_display_category(TOOL_DISPLAY_GROUP_PAINT), Category::Paint);
  EXPECT_EQ(brush_set_temp::tool_display_category(TOOL_DISPLAY_GROUP_SHARED),
            Category::Sculpt | Category::Paint);
}

/* The reported defect: a Paint-only tool that owns no brush (`brush_type` 0) must still filter the
 * shelf as Paint before the toolbar writes any workspace state, e.g. right after loading a file. */
TEST(asset_shelf_brush_set_temp, resolve_category_paint_only_tool_without_brush)
{
  EXPECT_EQ(brush_set_temp::resolve_category(Category::Paint,
                                             /*brush_type=*/0,
                                             /*stored_group=*/Category::Sculpt),
            Category::Paint);
}

TEST(asset_shelf_brush_set_temp, resolve_category_display_group_wins_over_brush_type)
{
  /* A tool drawn in the Sculpt group that uses a paint brush type is still Sculpt, exactly as the
   * toolbar shows it. */
  EXPECT_EQ(brush_set_temp::resolve_category(Category::Sculpt,
                                             SCULPT_BRUSH_TYPE_PAINT,
                                             Category::Paint),
            Category::Sculpt);
}

TEST(asset_shelf_brush_set_temp, resolve_category_brush_type_when_group_not_declared)
{
  /* Add-ons (and tools written before the display group existed) may only declare a brush type. */
  EXPECT_EQ(brush_set_temp::resolve_category(Category::Any, SCULPT_BRUSH_TYPE_PAINT, Category::Sculpt),
            Category::Paint);
  EXPECT_EQ(brush_set_temp::resolve_category(Category::Any, SCULPT_BRUSH_TYPE_DRAW, Category::Paint),
            Category::Sculpt);
  /* A mask brush is usable from both groups, so it decides nothing. */
  EXPECT_EQ(brush_set_temp::resolve_category(Category::Any, SCULPT_BRUSH_TYPE_MASK, Category::Paint),
            Category::Paint);
}

TEST(asset_shelf_brush_set_temp, resolve_category_shared_tool_follows_stored_group)
{
  const Category shared = Category::Sculpt | Category::Paint;
  EXPECT_EQ(brush_set_temp::resolve_category(shared, 0, Category::Paint), Category::Paint);
  EXPECT_EQ(brush_set_temp::resolve_category(shared, 0, Category::Sculpt), Category::Sculpt);
  /* Two workspaces keep independent stored groups, so the same tool resolves differently. */
  EXPECT_NE(brush_set_temp::resolve_category(shared, 0, Category::Paint),
            brush_set_temp::resolve_category(shared, 0, Category::Sculpt));
}

TEST(asset_shelf_brush_set_temp, resolve_category_nothing_declared_follows_stored_group)
{
  EXPECT_EQ(brush_set_temp::resolve_category(Category::Any, 0, Category::Sculpt), Category::Sculpt);
  EXPECT_EQ(brush_set_temp::resolve_category(Category::Any, 0, Category::Paint), Category::Paint);
  EXPECT_EQ(brush_set_temp::resolve_category(Category::Any, 0, Category::Any), Category::Any);
}

/** \} */

}  // namespace blender::ed::asset::shelf::tests
