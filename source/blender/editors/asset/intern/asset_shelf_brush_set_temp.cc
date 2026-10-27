/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * TEMPORARY: implementation of the Sculpt/Paint brush-set filtering, see the header for the
 * removal checklist and rationale.
 */

#include "ED_asset_shelf_brush_set_temp.hh"

#include <optional>

#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BKE_asset.hh"
#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_paint_types.hh"
#include "BKE_preferences.h"
#include "BKE_screen.hh"
#include "BKE_workspace.hh"

#include "BLI_utildefines.h"

#include "ED_asset_shelf.hh"

#include "DNA_asset_types.h"
#include "DNA_brush_enums.h"
#include "DNA_ID.h"
#include "DNA_object_enums.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_workspace_types.h"

#include "WM_toolsystem.hh"

namespace blender::ed::asset::shelf::brush_set_temp {

/* -------------------------------------------------------------------- */
/** \name Brush type classification
 * \{ */

/* The persistent #eAssetLibraryBrushSet bits and #Category are the same bits, converted by a cast. */
static_assert(int(Category::Sculpt) == ASSET_LIBRARY_BRUSH_SET_SCULPT);
static_assert(int(Category::Paint) == ASSET_LIBRARY_BRUSH_SET_PAINT);

/* A brush asset is classified by its brush type, not by a tool. The tool side is declared by the
 * tools themselves (#bToolRef_Runtime::display_group), so adding a tool does not require touching
 * this; the brush type to group mapping is shared with the kernel
 * (#bke::brush::sculpt_brush_type_in_paint_group). */
Category brush_type_categories(const int sculpt_brush_type)
{
  /* Mask brushes are selected through the mask target mode, which is available in both Sculpt and
   * Paint states, so they belong to both groups and pass either filter. */
  if (sculpt_brush_type == SCULPT_BRUSH_TYPE_MASK) {
    return Category::Sculpt | Category::Paint;
  }
  /* `eBrushSculptType` starts at 1; 0 and -1 mean "no / any brush type" (see
   * #bToolRef_Runtime::brush_type), which says nothing about a group. */
  if (sculpt_brush_type <= 0) {
    return Category::Any;
  }
  return bke::brush::sculpt_brush_type_in_paint_group(sculpt_brush_type) ? Category::Paint :
                                                                          Category::Sculpt;
}

Category tool_display_category(const eToolDisplayGroup display_group)
{
  switch (display_group) {
    case TOOL_DISPLAY_GROUP_SCULPT:
      return Category::Sculpt;
    case TOOL_DISPLAY_GROUP_PAINT:
      return Category::Paint;
    case TOOL_DISPLAY_GROUP_SHARED:
      return Category::Sculpt | Category::Paint;
    case TOOL_DISPLAY_GROUP_NONE:
      break;
  }
  return Category::Any;
}

Category resolve_category(const Category declared_group,
                          const int brush_type,
                          const Category stored_group)
{
  if (ELEM(declared_group, Category::Sculpt, Category::Paint)) {
    return declared_group;
  }
  if (declared_group == Category::Any) {
    /* Tools that predate the display group, and add-ons that declare only a brush type. */
    const Category from_brush = brush_type_categories(brush_type);
    if (ELEM(from_brush, Category::Sculpt, Category::Paint)) {
      return from_brush;
    }
  }
  /* Declared for both groups, or nothing to go by: the user's last explicit choice decides. */
  return stored_group;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt/Paint display state
 * \{ */

/** True for the Sculpt Mode brush shelf, the only shelf split into Sculpt/Paint display groups. */
static bool is_sculpt_brush_shelf_context(const bContext &C, const AssetShelf &shelf)
{
  const char *sculpt_shelf_idname = brush_shelf_idname_from_paint_mode(PaintMode::Sculpt);
  if (!shelf.type || !sculpt_shelf_idname || !STREQ(shelf.type->idname, sculpt_shelf_idname)) {
    return false;
  }
  const Object *object = CTX_data_active_object(&C);
  return object != nullptr && (object->mode & OB_MODE_SCULPT);
}

/** Group the user last picked with the toolbar's "Sculpt" / "Paint" buttons, per workspace. Only a
 * fallback for tools that declare no group of their own, so that the shelf keeps showing what the
 * toolbar shows for them. */
static Category stored_category(const bContext &C)
{
  const WorkSpace *workspace = CTX_wm_workspace(&C);
  if (!workspace || !workspace->runtime) {
    return Category::Any;
  }
  switch (workspace->runtime->sculpt_paint_tool_display) {
    case bke::eSculptPaintToolDisplay::Paint:
      return Category::Paint;
    case bke::eSculptPaintToolDisplay::Sculpt:
      return Category::Sculpt;
  }
  return Category::Any;
}

Category category_from_shelf(const bContext &C, const AssetShelf &shelf)
{
  if (!is_sculpt_brush_shelf_context(C, shelf)) {
    return Category::Any;
  }
  /* Priority: what the active tool declares, then the workspace's stored choice. Tools shown in
   * both groups declare nothing that names a single one, so they follow the stored choice, which
   * is what the toolbar shows for them. */
  Category declared_group = Category::Any;
  int brush_type = 0;
  if (const bToolRef_Runtime *tref_rt = WM_toolsystem_runtime_from_context(&C)) {
    declared_group = tool_display_category(tref_rt->display_group);
    /* Only meaningful when the tool is a brush tool; 0 otherwise means "no brush type". */
    if (tref_rt->flag & TOOLREF_FLAG_USE_BRUSHES) {
      brush_type = tref_rt->brush_type;
    }
  }
  return resolve_category(declared_group, brush_type, stored_category(C));
}

Category required_for_shelf(const bContext *C, const AssetShelf *shelf)
{
  if (!C || !shelf) {
    return Category::Any;
  }
  return category_from_shelf(*C, *shelf);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Library classification
 * \{ */

bool library_passes(const bUserAssetLibrary &library, const Category required)
{
  if (required == Category::Any) {
    return true;
  }
  /* Folders are containers, never selectable leaves; the selector prunes them when their subtree
   * has nothing to show. */
  if (library.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
    return true;
  }
  return flag_is_set(Category(library.brush_set), required);
}

bool essentials_passes(const UserDef &userdef, const Category required)
{
  if (required == Category::Any) {
    return true;
  }
  return flag_is_set(Category(userdef.essentials_brush_set), required);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset classification
 * \{ */

/* The library owning \a asset must pass \a category. Applies the per-library classification to
 * every asset, which is what filters the merged "All" library by owner. */
static bool asset_owner_library_passes(const asset_system::AssetRepresentation &asset,
                                const Category category)
{
  const std::optional<AssetLibraryReference> owner_ref = asset.owner_asset_library()
                                                             .library_reference();
  if (!owner_ref) {
    return true;
  }
  if (owner_ref->type == ASSET_LIBRARY_ESSENTIALS ||
      owner_ref->type == ASSET_LIBRARY_ONLINE_ESSENTIALS)
  {
    return essentials_passes(U, category);
  }
  if (owner_ref->type == ASSET_LIBRARY_CUSTOM) {
    const bUserAssetLibrary *owner_library = BKE_preferences_asset_library_find_from_ref(&U,
                                                                                         &*owner_ref);
    if (owner_library) {
      return library_passes(*owner_library, category);
    }
  }
  return true;
}

bool asset_passes(const asset_system::AssetRepresentation &asset, const Category category)
{
  if (category == Category::Any) {
    return true;
  }
  if (!asset_owner_library_passes(asset, category)) {
    return false;
  }

  /* Only brush assets are classified; anything else (shouldn't reach a brush shelf anyway) and
   * any brush without the metadata (e.g. written by an older version) pass unfiltered. A brush
   * that loses the metadata thus silently escapes the filter, see
   * #bke::brush::sculpt_brush_type_asset_key. */
  if (asset.get_id_type() != ID_BR) {
    return true;
  }
  const IDProperty *brush_type = BKE_asset_metadata_idprop_find(&asset.get_metadata(),
                                                                bke::brush::sculpt_brush_type_asset_key);
  if (!brush_type) {
    return true;
  }
  const int sculpt_brush_type = IDP_coerce_to_int_or_zero(brush_type);
  return flag_is_set(brush_type_categories(sculpt_brush_type), category);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Library reference validation
 * \{ */

void ensure_library_ref(AssetShelfSettings &settings, const Category category)
{
  if (category == Category::Any) {
    return;
  }

  const AssetLibraryReference &ref = settings.asset_library_reference;
  if (ref.type == ASSET_LIBRARY_ESSENTIALS || ref.type == ASSET_LIBRARY_ONLINE_ESSENTIALS) {
    if (!essentials_passes(U, category)) {
      settings.asset_library_reference = asset_system::all_library_reference();
    }
    return;
  }
  if (ref.type == ASSET_LIBRARY_CUSTOM) {
    const bUserAssetLibrary *library = BKE_preferences_asset_library_find_from_ref(&U, &ref);
    if (library && !library_passes(*library, category)) {
      settings.asset_library_reference = asset_system::all_library_reference();
    }
  }
}

/** \} */

}  // namespace blender::ed::asset::shelf::brush_set_temp
