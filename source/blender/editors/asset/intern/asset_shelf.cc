/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * General asset shelf code, mostly region callbacks, drawing and context stuff.
 */

#include <algorithm>
#include <cfloat>
#include <climits>

#include "AS_asset_catalog_path.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_function_ref.hh"
#include "BLI_listbase.hh"
#include "BLI_string_utf8.hh"

#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "ED_asset_list.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_tree_view.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_message.hh"

#include "ED_asset_shelf.hh"
#include "asset_shelf.hh"

namespace blender::ed::asset::shelf {

static int asset_shelf_default_tile_height();

void send_redraw_notifier(const bContext &C)
{
  WM_event_add_notifier(&C, NC_SPACE | ND_REGIONS_ASSET_SHELF, nullptr);
}

/* -------------------------------------------------------------------- */
/** \name Shelf Type
 * \{ */

static Vector<std::unique_ptr<AssetShelfType>> &static_shelf_types()
{
  static Vector<std::unique_ptr<AssetShelfType>> shelf_types;
  return shelf_types;
}

void type_register(std::unique_ptr<AssetShelfType> type)
{
  Vector<std::unique_ptr<AssetShelfType>> &shelf_types = static_shelf_types();
  shelf_types.append(std::move(type));
}

void type_unregister(const AssetShelfType &shelf_type)
{
  Vector<std::unique_ptr<AssetShelfType>> &shelf_types = static_shelf_types();
  auto *const it = std::find_if(shelf_types.begin(),
                                shelf_types.end(),
                                [&](const std::unique_ptr<AssetShelfType> &iter_type) {
                                  return iter_type.get() == &shelf_type;
                                });
  BLI_assert(it != shelf_types.end());

  shelf_types.remove(it - shelf_types.begin());
}

static bool type_poll_no_spacetype_check(const bContext &C, const AssetShelfType *shelf_type)
{
  if (!shelf_type) {
    return false;
  }

#ifndef NDEBUG
  const Vector<std::unique_ptr<AssetShelfType>> &shelf_types = static_shelf_types();
  BLI_assert_msg(std::find_if(shelf_types.begin(),
                              shelf_types.end(),
                              [&](const std::unique_ptr<AssetShelfType> &type) {
                                return type.get() == shelf_type;
                              }) != shelf_types.end(),
                 "Asset shelf type is not registered");
#endif

  return !shelf_type->poll || shelf_type->poll(&C, shelf_type);
}

bool type_poll_for_popup(const bContext &C, const AssetShelfType *shelf_type)
{
  return type_poll_no_spacetype_check(C, shelf_type);
}

/**
 * Poll an asset shelf type for display as a permanent region in a space of a given type (the
 * type's #bl_space_type).
 *
 * Popup asset shelves should use #type_poll_for_popup() instead.
 */
static bool type_poll_for_non_popup(const bContext &C,
                                    const AssetShelfType *shelf_type,
                                    const int space_type)
{
  if (!shelf_type) {
    return false;
  }
  if (shelf_type->space_type && (space_type != shelf_type->space_type)) {
    return false;
  }

  return type_poll_no_spacetype_check(C, shelf_type);
}

bool type_asset_poll(const AssetShelfType &shelf_type,
                     const asset_system::AssetRepresentation &asset)
{

  if (shelf_type.id_types_prefilter != 0) {
    const uint64_t id_filter = BKE_idtype_idcode_to_idfilter(asset.get_id_type());
    if ((shelf_type.id_types_prefilter & id_filter) == 0) {
      return false;
    }
  }

  if (shelf_type.asset_poll && !shelf_type.asset_poll(&shelf_type, &asset)) {
    return false;
  }

  return true;
}

AssetShelfType *type_find_from_idname(const StringRef idname)
{
  for (const std::unique_ptr<AssetShelfType> &shelf_type : static_shelf_types()) {
    if (idname == shelf_type->idname) {
      return shelf_type.get();
    }
  }
  return nullptr;
}

AssetShelfType *ensure_shelf_has_type(AssetShelf &shelf)
{
  if (shelf.type) {
    return shelf.type;
  }

  for (const std::unique_ptr<AssetShelfType> &shelf_type : static_shelf_types()) {
    if (STREQ(shelf.idname, shelf_type->idname)) {
      shelf.type = shelf_type.get();
      return shelf_type.get();
    }
  }

  return nullptr;
}

AssetShelf *create_shelf_from_type(AssetShelfType &type)
{
  AssetShelf *shelf = MEM_new<AssetShelf>(__func__);
  *shelf = dna::shallow_zero_initialize();
  shelf->settings.preview_size = type.default_preview_size ? type.default_preview_size :
                                                             ASSET_SHELF_PREVIEW_SIZE_DEFAULT;
  shelf->settings.asset_library_reference = asset_system::all_library_reference();
  shelf->type = &type;
  shelf->preferred_row_count = 1;
  STRNCPY_UTF8(shelf->idname, type.idname);
  return shelf;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Active Shelf Management
 * \{ */

/**
 * Activating a shelf means assigning it to #RegionAssetShelf.active_shelf and (re-)inserting it at
 * the beginning of the #RegionAssetShelf.shelves list. This implies that after calling this, \a
 * shelf is guaranteed to be owned by the shelves list.
 */
static void activate_shelf(RegionAssetShelf &shelf_regiondata, AssetShelf &shelf)
{
  shelf_regiondata.active_shelf = &shelf;
  BLI_assert(BLI_findindex(&shelf_regiondata.shelves, &shelf) > -1);
  BLI_remlink(&shelf_regiondata.shelves, &shelf);
  BLI_addhead(&shelf_regiondata.shelves, &shelf);
}

/**
 * Determine and set the currently active asset shelf, creating a new shelf if needed.
 *
 * The heuristic works as follows:
 * 1) If the currently active shelf is still valid (poll succeeds), keep it active.
 * 2) Otherwise, check for previously activated shelves in \a shelf_regiondata and activate the
 *    first valid one (first with a succeeding poll).
 * 3) If none is valid, check all shelf-types available for \a space_type, create a new shelf for
 *    the first type that is valid (poll succeeds), and activate it.
 * 4) If no shelf-type is valid, #RegionAssetShelf.active_shelf is set to null.
 *
 * When activating a shelf, it is moved to the beginning of the #RegionAssetShelf.shelves list, so
 * that recently activated shelves are also the first ones to be reactivated.
 *
 * The returned shelf is guaranteed to have its #AssetShelf.type pointer set.
 *
 * \param on_create: Function called when a new asset shelf is created (case 3).
 *
 * \return A non-owning pointer to the now active shelf. Might be null if no shelf is valid in
 *         current context (all polls failed).
 */
static AssetShelf *update_active_shelf(const bContext &C,
                                       const eSpace_Type space_type,
                                       RegionAssetShelf &shelf_regiondata,
                                       FunctionRef<void(AssetShelf &new_shelf)> on_create,
                                       FunctionRef<void(AssetShelf &shelf)> on_reactivate)
{
  /* NOTE: Don't access #AssetShelf.type directly, use #type_ensure(). */

  /* Case 1: */
  if (shelf_regiondata.active_shelf &&
      type_poll_for_non_popup(
          C, ensure_shelf_has_type(*shelf_regiondata.active_shelf), space_type))
  {
    /* Not a strong precondition, but if this is wrong something weird might be going on. */
    BLI_assert(shelf_regiondata.active_shelf == shelf_regiondata.shelves.first);
    return shelf_regiondata.active_shelf;
  }

  /* Case 2 (no active shelf or the poll of it isn't succeeding anymore. Poll all shelf types to
   * determine a new active one): */
  for (AssetShelf &shelf : shelf_regiondata.shelves) {
    if (&shelf == shelf_regiondata.active_shelf) {
      continue;
    }

    if (type_poll_for_non_popup(C, ensure_shelf_has_type(shelf), space_type)) {
      /* Found a valid previously activated shelf, reactivate it. */
      activate_shelf(shelf_regiondata, shelf);
      if (on_reactivate) {
        on_reactivate(shelf);
      }
      return &shelf;
    }
  }

  /* Case 3: */
  for (const std::unique_ptr<AssetShelfType> &shelf_type : static_shelf_types()) {
    if (type_poll_for_non_popup(C, shelf_type.get(), space_type)) {
      AssetShelf *new_shelf = create_shelf_from_type(*shelf_type);
      BLI_addhead(&shelf_regiondata.shelves, new_shelf);
      /* Moves ownership to the regiondata. */
      activate_shelf(shelf_regiondata, *new_shelf);
      if (on_create) {
        on_create(*new_shelf);
      }
      return new_shelf;
    }
  }

  shelf_regiondata.active_shelf = nullptr;
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset Shelf Regions
 * \{ */

void *region_duplicate(void *regiondata)
{
  const RegionAssetShelf *shelf_regiondata = static_cast<RegionAssetShelf *>(regiondata);
  if (!shelf_regiondata) {
    return nullptr;
  }

  return regiondata_duplicate(shelf_regiondata);
}

void region_free(ARegion *region)
{
  RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(*region);
  if (shelf_regiondata) {
    regiondata_free(shelf_regiondata);
  }
  region->regiondata = nullptr;
}

/**
 * Check if there is any asset shelf type in this space returning true in its poll. If not, no
 * asset shelf region should be displayed.
 */
static bool asset_shelf_space_poll(const bContext *C, const SpaceLink *space_link)
{
  /* Is there any asset shelf type registered that returns true for it's poll? */
  for (const std::unique_ptr<AssetShelfType> &shelf_type : static_shelf_types()) {
    if (type_poll_for_non_popup(*C, shelf_type.get(), space_link->spacetype)) {
      return true;
    }
  }

  return false;
}

bool regions_poll(const RegionPollParams *params)
{
  return asset_shelf_space_poll(params->context,
                                static_cast<SpaceLink *>(params->area->spacedata.first));
}

static void asset_shelf_region_listen(const wmRegionListenerParams *params)
{
  ARegion *region = params->region;
  const wmNotifier *wmn = params->notifier;

  switch (wmn->category) {
    case NC_SPACE:
      if (wmn->data == ND_REGIONS_ASSET_SHELF) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SCENE:
      /* Asset shelf polls typically check the mode. */
      if (ELEM(wmn->data, ND_MODE)) {
        ED_region_tag_redraw(region);
      }
      break;
    case NC_ASSET:
      ED_region_tag_redraw(region);
      break;
  }
}

void region_listen(const wmRegionListenerParams *params)
{
  if (list::listen(params->notifier)) {
    ED_region_tag_redraw_no_rebuild(params->region);
  }
  /* If the asset list didn't catch the notifier, let the region itself listen. */
  else {
    asset_shelf_region_listen(params);
  }
}

void region_message_subscribe(const wmRegionMessageSubscribeParams *params)
{
  wmMsgBus *mbus = params->message_bus;
  WorkSpace *workspace = params->workspace;
  ARegion *region = params->region;

  wmMsgSubscribeValue msg_sub_value_region_tag_redraw{};
  msg_sub_value_region_tag_redraw.owner = region;
  msg_sub_value_region_tag_redraw.user_data = region;
  msg_sub_value_region_tag_redraw.notify = ED_region_do_msg_notify_tag_redraw;
  WM_msg_subscribe_rna_prop(
      mbus, &workspace->id, workspace, WorkSpace, tools, &msg_sub_value_region_tag_redraw);

  {
    wmMsgSubscribeValue msg_sub_value_region_clear_remote_libraries{};
    msg_sub_value_region_clear_remote_libraries.owner = region;
    msg_sub_value_region_clear_remote_libraries.user_data = region;
    msg_sub_value_region_clear_remote_libraries.notify = [](/* Follow wmMsgNotifyFn spec */
                                                            bContext *C,
                                                            wmMsgSubscribeKey * /*msg_key*/,
                                                            wmMsgSubscribeValue *msg_val) {
      ARegion *region = static_cast<ARegion *>(msg_val->owner);
      RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(*region);
      AssetShelf *active_shelf = shelf_regiondata->active_shelf;
      if (blender::asset_system::is_or_contains_remote_libraries(
              active_shelf->settings.asset_library_reference))
      {
        asset::list::clear(&active_shelf->settings.asset_library_reference, C);
      }
    };
    WM_msg_subscribe_rna_prop(mbus,
                              nullptr,
                              &U,
                              PreferencesSystem,
                              use_online_access,
                              &msg_sub_value_region_clear_remote_libraries);
    WM_msg_subscribe_rna_prop(mbus,
                              nullptr,
                              &U,
                              PreferencesExperimental,
                              use_remote_asset_libraries,
                              &msg_sub_value_region_clear_remote_libraries);
  }
}

void region_init(wmWindowManager *wm, ARegion *region)
{
  /* Region-data should've been created by a previously called #region_on_poll_success(). */
  RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(*region);
  BLI_assert_msg(
      shelf_regiondata,
      "Region-data should've been created by a previously called `region_on_poll_success()`.");

  AssetShelf *active_shelf = shelf_regiondata->active_shelf;

  view2d_region_reinit(&region->v2d, ui::V2D_COMMONVIEW_PANELS_UI, region->winx, region->winy);

  wmKeyMap *keymap = WM_keymap_ensure(
      wm->runtime->defaultconf, "View2D Buttons List", SPACE_EMPTY, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);

  region->v2d.scroll = V2D_SCROLL_RIGHT | V2D_SCROLL_VERTICAL_HIDE;
  region->v2d.keepzoom |= V2D_LOCKZOOM_X | V2D_LOCKZOOM_Y;
  region->v2d.keepofs |= V2D_KEEPOFS_Y;

  region->v2d.flag |= V2D_SNAP_TO_PAGESIZE_Y;
  region->v2d.page_size_y = active_shelf ? tile_height(active_shelf->settings) :
                                           asset_shelf_default_tile_height();

  /* Ensure the view is snapped to a page still, especially for DPI changes. */
  ui::view2d_offset_y_snap_to_closest_page(&region->v2d);
}

static int main_region_padding_y()
{
  const uiStyle *style = ui::style_get_dpi();
  return style->buttonspacey / 2;
}

static int main_region_padding_x()
{
  /* Use the same as the height, equal padding looks nice. */
  return main_region_padding_y();
}

static int current_tile_draw_height(const ARegion *region)
{
  const RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(
      *region);
  const float aspect = BLI_rctf_size_y(&region->v2d.cur) /
                       (BLI_rcti_size_y(&region->v2d.mask) + 1);

  /* It can happen that this function is called before the region is actually initialized, when
   * user clicks & drags slightly on the 'up arrow' icon of the shelf. */
  const AssetShelf *active_shelf = shelf_regiondata ? shelf_regiondata->active_shelf : nullptr;
  return (active_shelf ? tile_height(active_shelf->settings) : asset_shelf_default_tile_height()) /
         (IS_EQF(aspect, 0) ? 1.0f : aspect);
}

/**
 * How many rows fit into the region (accounting for padding).
 */
static int calculate_row_count_from_tile_draw_height(const int region_height_scaled,
                                                     const int tile_draw_height)
{
  return std::max(1, int((region_height_scaled - 2 * main_region_padding_y()) / tile_draw_height));
}

static int calculate_scaled_region_height_from_row_count(const int row_count,
                                                         const int tile_draw_height)
{
  return (row_count * tile_draw_height + 2 * main_region_padding_y());
}

int region_snap(const ARegion *region, const int size, const int axis)
{
  /* Only on Y axis. */
  if (axis != 1) {
    return size;
  }

  /* Using scaled values only simplifies things. Simply divide the result by the scale again. */

  const int tile_height = current_tile_draw_height(region);

  const int row_count = calculate_row_count_from_tile_draw_height(size * UI_SCALE_FAC,
                                                                  tile_height);

  const int new_size_scaled = calculate_scaled_region_height_from_row_count(row_count,
                                                                            tile_height);
  return new_size_scaled / UI_SCALE_FAC;
}

/**
 * Ensure the region height matches the preferred row count (see #AssetShelf.preferred_row_count)
 * as closely as possible while still fitting within the area. In any case, this will ensure the
 * region height is snapped to a multiple of the row count (plus region padding).
 */
static void region_resize_to_preferred(ScrArea *area, ARegion *region)
{
  const RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(
      *region);
  const AssetShelf *active_shelf = shelf_regiondata->active_shelf;

  BLI_assert(active_shelf->preferred_row_count > 0);
  const int tile_height = current_tile_draw_height(region);

  /* Prevent the AssetShelf from getting too high (and thus being hidden) in case many rows are
   * used and preview size is increased. */
  const int size_y_avail = ED_area_max_regionsize(area, region, AE_TOP_TO_BOTTOMRIGHT);
  const short int max_row_count = calculate_row_count_from_tile_draw_height(
      size_y_avail * UI_SCALE_FAC, tile_height);

  const int new_size_y = calculate_scaled_region_height_from_row_count(
                             std::min(max_row_count, active_shelf->preferred_row_count),
                             tile_height) /
                         UI_SCALE_FAC;

  if (region->sizey != new_size_y) {
    region->sizey = new_size_y;
    ED_area_tag_region_size_update(area, region);
  }
}

void region_on_user_resize(const ARegion *region)
{
  const RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(
      *region);
  AssetShelf *active_shelf = shelf_regiondata->active_shelf;
  if (!active_shelf) {
    return;
  }

  const int tile_height = current_tile_draw_height(region);
  active_shelf->preferred_row_count = calculate_row_count_from_tile_draw_height(
      region->sizey * UI_SCALE_FAC, tile_height);
}

int tile_width(const AssetShelfSettings &settings)
{
  return ui::preview_tile_size_x(settings.preview_size);
}

int tile_height(const AssetShelfSettings &settings)
{
  return (settings.display_flag & ASSETSHELF_SHOW_NAMES) ?
             ui::preview_tile_size_y(settings.preview_size) :
             ui::preview_tile_size_y_no_label(settings.preview_size);
}

static int asset_shelf_default_tile_height()
{
  return ui::preview_tile_size_x(ASSET_SHELF_PREVIEW_SIZE_DEFAULT);
}

int region_prefsizey()
{
  /* One row by default (plus padding). */
  return asset_shelf_default_tile_height() + 2 * main_region_padding_y();
}

void region_layout(const bContext *C, ARegion *region)
{
  RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(*region);
  BLI_assert_msg(
      shelf_regiondata,
      "Region-data should've been created by a previously called `region_on_poll_success()`.");

  AssetShelf *active_shelf = shelf_regiondata->active_shelf;
  if (!active_shelf) {
    return;
  }

  settings_ensure_valid_library_ref(active_shelf->settings);

  ui::Block *block = block_begin(C, region, __func__, ui::EmbossType::Emboss);

  const uiStyle *style = ui::style_get_dpi();
  const int padding_y = main_region_padding_y();
  const int padding_x = main_region_padding_x();
  ui::Layout &layout = ui::block_layout(block,
                                        ui::LayoutDirection::Vertical,
                                        ui::LayoutType::Panel,
                                        padding_x,
                                        -padding_y,
                                        region->winx - 2 * padding_x,
                                        0,
                                        0,
                                        style);

  build_asset_view(layout, active_shelf->settings.asset_library_reference, *active_shelf, *C);

  int layout_height = ui::block_layout_resolve(block).y;
  BLI_assert(layout_height <= 0);
  ui::view2d_totRect_set(&region->v2d, region->winx - 1, layout_height - padding_y);
  ui::view2d_curRect_validate(&region->v2d);

  region_resize_to_preferred(CTX_wm_area(C), region);

  /* View2D matrix might have changed due to dynamic sized regions.
   * Without this, tooltips jump around, see #129347. Reason is that #button_tooltip_refresh() is
   * called as part of #block_end(), so the block's window matrix needs to be up-to-date. */
  {
    ui::view2d_view_ortho(&region->v2d);
    ui::blocklist_update_window_matrix(C, &region->runtime->uiblocks);
  }

  block_end(C, block);
}

void region_draw(const bContext *C, ARegion *region)
{
  ED_region_clear(C, region, TH_BACK);

  /* Set view2d view matrix for scrolling. */
  ui::view2d_view_ortho(&region->v2d);

  /* View2D matrix might have changed due to dynamic sized regions. */
  ui::blocklist_update_window_matrix(C, &region->runtime->uiblocks);

  ui::blocklist_draw(C, &region->runtime->uiblocks);

  /* Restore view matrix. */
  ui::view2d_view_restore(C);

  ui::view2d_scrollers_draw(&region->v2d, nullptr);
}

void region_on_poll_success(const bContext *C, ARegion *region)
{
  RegionAssetShelf *shelf_regiondata = RegionAssetShelf::ensure_from_asset_shelf_region(*region);
  if (!shelf_regiondata) {
    BLI_assert_unreachable();
    return;
  }

  const int old_region_flag = region->flag;

  ScrArea *area = CTX_wm_area(C);
  update_active_shelf(
      *C,
      eSpace_Type(area->spacetype),
      *shelf_regiondata,
      /*on_create=*/
      [&](AssetShelf &new_shelf) {
        /* Set region visibility for first time shelf is created (`'DEFAULT_VISIBLE'` option). */
        SET_FLAG_FROM_TEST(region->flag,
                           (new_shelf.type->flag & ASSET_SHELF_TYPE_FLAG_DEFAULT_VISIBLE) == 0,
                           RGN_FLAG_HIDDEN);
      },
      /*on_reactivate=*/
      [&](AssetShelf &shelf) {
        /* Restore region visibility from previous asset shelf instantiation when reactivating. */
        SET_FLAG_FROM_TEST(
            region->flag, shelf.instance_flag & ASSETSHELF_REGION_IS_HIDDEN, RGN_FLAG_HIDDEN);
      });

  if (old_region_flag != region->flag) {
    ED_region_visibility_change_update(const_cast<bContext *>(C), area, region);
  }

  if (shelf_regiondata->active_shelf) {
    /* Remember current visibility state of the region in the shelf, so we can restore it on
     * reactivation. */
    SET_FLAG_FROM_TEST(shelf_regiondata->active_shelf->instance_flag,
                       region->flag & (RGN_FLAG_HIDDEN | RGN_FLAG_HIDDEN_BY_USER),
                       ASSETSHELF_REGION_IS_HIDDEN);
  }
}

void header_region_listen(const wmRegionListenerParams *params)
{
  asset_shelf_region_listen(params);
}

void header_region_init(wmWindowManager * /*wm*/, ARegion *region)
{
  ED_region_header_init(region);
  region->alignment |= RGN_SPLIT_SCALE_PREV;
  region->flag |= RGN_FLAG_RESIZE_RESPECT_BUTTON_SECTIONS;
}

void header_region(const bContext *C, ARegion *region)
{
  ED_region_header_with_button_sections(C, region, ui::ButtonSectionsAlign::Bottom);
}

int header_region_size()
{
  /* Use a height that lets widgets sit just on top of the separator line drawn at the lower edge
   * of the region (widgets will be centered).
   *
   * Note that this is usually a bit less than the header size. The asset shelf tends to look like
   * a separate area, so making the shelf header smaller than a header helps. */
  return UI_UNIT_Y + (UI_BUTTON_SECTION_SEPERATOR_LINE_WITH * 2);
}

void region_blend_read_data(BlendDataReader *reader, ARegion *region)
{
  RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(*region);
  if (!shelf_regiondata) {
    return;
  }
  regiondata_blend_read_data(reader, &shelf_regiondata);
  region->regiondata = shelf_regiondata;
}

void region_blend_write(BlendWriter *writer, ARegion *region)
{
  RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(*region);
  if (!shelf_regiondata) {
    return;
  }
  regiondata_blend_write(writer, shelf_regiondata);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset Shelf Context
 * \{ */

AssetShelf *active_shelf_from_area(const ScrArea *area)
{
  const ARegion *shelf_region = BKE_area_find_region_type(area, RGN_TYPE_ASSET_SHELF);
  if (!shelf_region) {
    /* Called in wrong context, area doesn't have a shelf. */
    return nullptr;
  }

  if (shelf_region->flag & RGN_FLAG_POLL_FAILED) {
    /* Don't return data when the region "doesn't exist" (poll failed). */
    return nullptr;
  }

  const RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(
      *shelf_region);
  if (!shelf_regiondata) {
    return nullptr;
  }

  return shelf_regiondata->active_shelf;
}

int context(const bContext *C, const char *member, bContextDataResult *result)
{
  static const char *context_dir[] = {
      "asset_shelf",
      "asset_library_reference",
      "asset",
      nullptr,
  };

  if (CTX_data_dir(member)) {
    CTX_data_dir_set(result, context_dir);
    return CTX_RESULT_OK;
  }

  bScreen *screen = CTX_wm_screen(C);

  if (CTX_data_equals(member, "asset_shelf")) {
    AssetShelf *active_shelf = active_shelf_from_area(CTX_wm_area(C));
    if (!active_shelf) {
      return CTX_RESULT_NO_DATA;
    }

    CTX_data_pointer_set(result, &screen->id, RNA_AssetShelf, active_shelf);
    return CTX_RESULT_OK;
  }

  if (CTX_data_equals(member, "asset_library_reference")) {
    AssetShelf *active_shelf = active_shelf_from_area(CTX_wm_area(C));
    if (!active_shelf) {
      return CTX_RESULT_NO_DATA;
    }
    AssetLibraryReference &library_ref = settings_ensure_valid_library_ref(active_shelf->settings);
    CTX_data_pointer_set(result, &screen->id, RNA_AssetLibraryReference, &library_ref);
    return CTX_RESULT_OK;
  }

  if (CTX_data_equals(member, "asset")) {
    const ARegion *region = CTX_wm_region(C);
    const ui::Button *but = ui::region_views_find_active_item_but(region);
    if (!but) {
      return CTX_RESULT_NO_DATA;
    }

    const bContextStore *but_context = button_context_get(but);
    if (!but_context) {
      return CTX_RESULT_NO_DATA;
    }

    const PointerRNA *asset_ptr = CTX_store_ptr_lookup(
        but_context, "asset", RNA_AssetRepresentation);
    if (!asset_ptr) {
      return CTX_RESULT_NO_DATA;
    }

    CTX_data_pointer_set_ptr(result, asset_ptr);
    return CTX_RESULT_OK;
  }

  return CTX_RESULT_MEMBER_NOT_FOUND;
}

static PointerRNA active_shelf_ptr_from_context(const bContext *C)
{
  return CTX_data_pointer_get_type(C, "asset_shelf", RNA_AssetShelf);
}

AssetShelf *active_shelf_from_context(const bContext *C)
{
  PointerRNA shelf_settings_ptr = active_shelf_ptr_from_context(C);
  return static_cast<AssetShelf *>(shelf_settings_ptr.data);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Catalog toggle buttons
 * \{ */

static ui::Button *add_tab_button(ui::Block &block, StringRefNull name)
{
  const uiStyle *style = ui::style_get_dpi();
  const int string_width = ui::fontstyle_string_width(&style->widget, name.c_str());
  const int pad_x = UI_UNIT_X * 0.3f;
  const int but_width = std::min(string_width + 2 * pad_x, UI_UNIT_X * 8);

  ui::Button *but = uiDefBut(
      &block,
      ui::ButtonType::Tab,
      name,
      0,
      0,
      but_width,
      UI_UNIT_Y,
      nullptr,
      0,
      0,
      TIP_("Enable catalog, making contained assets visible in the asset shelf"));

  button_drawflag_enable(but, ui::BUT_ALIGN_DOWN);
  button_flag_disable(but, ui::BUT_UNDO);

  return but;
}

static void add_catalog_tabs(AssetShelf &shelf, ui::Layout &layout)
{
  ui::Block *block = layout.block();
  AssetShelfSettings &shelf_settings = shelf.settings;

  /* "All" tab. */
  {
    ui::Button *but = add_tab_button(*block, IFACE_("All"));
    button_func_set(but, [&shelf_settings](bContext &C) {
      settings_set_all_catalog_active(shelf_settings);
      send_redraw_notifier(C);
    });
    button_func_pushed_state_set(but, [&shelf_settings](const ui::Button &) -> bool {
      return settings_is_all_catalog_active(shelf_settings);
    });
  }

  layout.separator();

  /* Right-click menu for reordering a catalog tab (see #catalog_tab_context_menu_register()). */
  MenuType *tab_menu = WM_menutype_find("ASSETSHELF_MT_catalog_tab_context_menu", false);

  /* Regular catalog tabs. */
  int catalog_index = 0;
  settings_foreach_enabled_catalog_path(shelf, [&](const asset_system::AssetCatalogPath &path) {
    ui::Button *but = add_tab_button(*block, path.name());

    button_func_set(but, [&shelf_settings, path](bContext &C) {
      settings_set_active_catalog(shelf_settings, path);
      send_redraw_notifier(C);
    });
    button_func_pushed_state_set(but, [&shelf_settings, path](const ui::Button &) -> bool {
      return settings_is_active_catalog(shelf_settings, path);
    });

    /* Enable drag & drop reordering of the tab (see #catalog_tabs_drag_drop_register()). The
     * dragged tab is identified by its index in the enabled catalog paths; the drop target reads
     * the same index back from the button context. */
    button_drag_set_asset_shelf_catalog(but, catalog_index);
    button_context_int_set(block, but, "asset_shelf_catalog_index", catalog_index);
    if (tab_menu) {
      ui::button_tab_menu_set(but, tab_menu);
    }
    catalog_index++;
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Catalog Tab Reordering (Drag & Drop)
 *
 * Lets the user reorder the catalog tabs in the asset shelf header by dragging them. The tab
 * buttons are made draggable in #add_catalog_tabs() (carrying the dragged catalog's index) and each
 * tab stores its index in the button context ("asset_shelf_catalog_index"). A drop-box registered
 * in the global "User Interface" drop-box map (so it is available in the header region) handles the
 * drop and runs #ASSET_OT_shelf_catalog_reorder.
 * \{ */

/**
 * Find the catalog tab under the cursor and return its index, or -1 if the cursor isn't over a
 * catalog tab. \a r_after is set to true when the cursor is in the right half of the tab (i.e. the
 * dragged tab should be inserted after it). \a r_tab_rect, when given, receives the tab's rectangle
 * in window coordinates (only written when a tab is found).
 */
static int catalog_tab_index_under_cursor(const bContext &C,
                                          const wmEvent &event,
                                          bool &r_after,
                                          rctf *r_tab_rect = nullptr)
{
  r_after = false;

  const ARegion *region = CTX_wm_region(&C);
  if (!region) {
    return -1;
  }
  ui::Button *but = ui::but_find_mouse_over(region, &event);
  if (!but) {
    return -1;
  }
  const std::optional<int64_t> index = ui::button_context_int_get(but, "asset_shelf_catalog_index");
  if (!index) {
    return -1;
  }

  const rctf rect = ui::button_screen_rect(but, region);
  const float center_x = (rect.xmin + rect.xmax) * 0.5f;
  r_after = event.xy[0] > center_x;
  if (r_tab_rect) {
    *r_tab_rect = rect;
  }
  return int(*index);
}

static bool catalog_tab_reorder_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (drag->type != WM_DRAG_ASSET_SHELF_CATALOG) {
    return false;
  }
  const ARegion *region = CTX_wm_region(C);
  if (!region || region->regiontype != RGN_TYPE_ASSET_SHELF_HEADER) {
    return false;
  }
  ui::Button *but = ui::but_find_mouse_over(region, event);
  if (!but) {
    return false;
  }
  return ui::button_context_int_get(but, "asset_shelf_catalog_index").has_value();
}

static void catalog_tab_reorder_drop_copy(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  const wmDragAssetShelfCatalog *catalog_drag = WM_drag_get_asset_shelf_catalog_data(drag);
  RNA_int_set(drop->ptr, "from_index", catalog_drag->index);

  int to_index = -1;
  const wmWindow *win = CTX_wm_window(C);
  if (win && win->runtime->eventstate) {
    bool after = false;
    const int target_index = catalog_tab_index_under_cursor(
        *C, *win->runtime->eventstate, after);
    if (target_index >= 0) {
      to_index = target_index + (after ? 1 : 0);
    }
  }
  RNA_int_set(drop->ptr, "to_index", to_index);
}

static std::string catalog_tab_reorder_drop_tooltip(bContext * /*C*/,
                                                    wmDrag * /*drag*/,
                                                    const int /*xy*/[2],
                                                    wmDropBox * /*drop*/)
{
  return TIP_("Reorder catalog tab");
}

/**
 * Draw a vertical insertion line in the gap where the dragged catalog tab would be dropped, so the
 * user sees the resulting position before releasing. Mirrors the insertion logic in
 * #catalog_tab_reorder_drop_copy() so the indicator matches the actual drop.
 */
static void catalog_tab_reorder_draw_in_view(bContext *C,
                                             wmWindow *win,
                                             wmDrag *drag,
                                             const int /*xy*/[2])
{
  const wmDragAssetShelfCatalog *catalog_drag = WM_drag_get_asset_shelf_catalog_data(drag);
  if (!catalog_drag || !win->runtime->eventstate) {
    return;
  }
  const ARegion *region = CTX_wm_region(C);
  if (!region) {
    return;
  }

  bool after = false;
  rctf tab_rect;
  const int target_index = catalog_tab_index_under_cursor(
      *C, *win->runtime->eventstate, after, &tab_rect);
  if (target_index < 0) {
    return;
  }

  const int from_index = catalog_drag->index;
  const int to_index = target_index + (after ? 1 : 0);
  /* Dropping a tab onto its own position leaves the order unchanged; don't draw a misleading line. */
  if (to_index == from_index || to_index == from_index + 1) {
    return;
  }

  /* The draw buffer uses the region's viewport but sets up no transform, so configure region pixel
   * space here. #button_screen_rect() returns window coordinates, so offset into region-local
   * coordinates to match. */
  wmOrtho2_region_pixelspace(region);

  const float x = (after ? tab_rect.xmax : tab_rect.xmin) - region->winrct.xmin;
  const float ymin = tab_rect.ymin - region->winrct.ymin;
  const float ymax = tab_rect.ymax - region->winrct.ymin;
  const float half_width = U.pixelsize;

  rctf line_rect;
  line_rect.xmin = x - half_width;
  line_rect.xmax = x + half_width;
  line_rect.ymin = ymin;
  line_rect.ymax = ymax;

  float color[4];
  ui::theme::get_color_blend_4f(TH_TEXT, TH_BACK, 0.4f, color);
  color[3] = 1.0f;

  ui::draw_roundbox_corner_set(ui::CNR_NONE);
  ui::draw_roundbox_4fv(&line_rect, true, 0.0f, color);
}

static bool catalog_tab_reorder_poll(bContext *C)
{
  return active_shelf_from_context(C) != nullptr;
}

static wmOperatorStatus catalog_tab_reorder_exec(bContext *C, wmOperator *op)
{
  AssetShelf *shelf = active_shelf_from_context(C);
  if (!shelf) {
    return OPERATOR_CANCELLED;
  }

  const int from_index = RNA_int_get(op->ptr, "from_index");
  const int to_index = RNA_int_get(op->ptr, "to_index");
  if (to_index < 0) {
    return OPERATOR_CANCELLED;
  }

  if (!settings_reorder_catalog_path(*shelf, from_index, to_index)) {
    return OPERATOR_CANCELLED;
  }

  send_redraw_notifier(*C);
  return OPERATOR_FINISHED;
}

static void ASSET_OT_shelf_catalog_reorder(wmOperatorType *ot)
{
  ot->name = "Reorder Asset Shelf Catalog Tab";
  ot->description = "Change the order of a catalog tab in the asset shelf";
  ot->idname = "ASSET_OT_shelf_catalog_reorder";

  ot->exec = catalog_tab_reorder_exec;
  ot->poll = catalog_tab_reorder_poll;

  ot->flag = OPTYPE_INTERNAL;

  RNA_def_int(ot->srna,
              "from_index",
              -1,
              -1,
              INT_MAX,
              "From Index",
              "Index of the catalog tab to move",
              -1,
              INT_MAX);
  RNA_def_int(ot->srna,
              "to_index",
              -1,
              -1,
              INT_MAX,
              "To Index",
              "Index to insert the dragged catalog tab in front of",
              -1,
              INT_MAX);
}

/**
 * Right-click context menu for a catalog tab, offering to move it one step towards the front or the
 * back. The clicked tab's index is read from its button context ("asset_shelf_catalog_index", set in
 * #add_catalog_tabs()); the menu reuses #ASSET_OT_shelf_catalog_reorder with explicit indices.
 */
static void catalog_tab_context_menu_draw(const bContext *C, Menu *menu)
{
  ui::Layout &layout = *menu->layout;

  const std::optional<int64_t> index = CTX_data_int_get(C, "asset_shelf_catalog_index");
  AssetShelf *shelf = active_shelf_from_context(C);
  if (!index || !shelf) {
    return;
  }
  const int from_index = int(*index);

  int count = 0;
  settings_foreach_enabled_catalog_path(
      *shelf, [&count](const asset_system::AssetCatalogPath & /*path*/) { count++; });

  /* Move one position towards the front. The reorder operator's "to_index" is the position to
   * insert in front of (see #settings_reorder_catalog_path()). */
  {
    ui::Layout &row = layout.row(false);
    row.enabled_set(from_index > 0);
    PointerRNA op_ptr = row.op(
        "ASSET_OT_shelf_catalog_reorder", IFACE_("Move Left"), ICON_TRIA_LEFT);
    RNA_int_set(&op_ptr, "from_index", from_index);
    RNA_int_set(&op_ptr, "to_index", from_index - 1);
  }

  /* Move one position towards the back. Inserting in front of the tab after the next one moves the
   * dragged tab one slot to the right. */
  {
    ui::Layout &row = layout.row(false);
    row.enabled_set(from_index < count - 1);
    PointerRNA op_ptr = row.op(
        "ASSET_OT_shelf_catalog_reorder", IFACE_("Move Right"), ICON_TRIA_RIGHT);
    RNA_int_set(&op_ptr, "from_index", from_index);
    RNA_int_set(&op_ptr, "to_index", from_index + 2);
  }
}

static void catalog_tab_context_menu_register()
{
  MenuType *mt = MEM_new_zeroed<MenuType>(__func__);
  STRNCPY_UTF8(mt->idname, "ASSETSHELF_MT_catalog_tab_context_menu");
  STRNCPY_UTF8(mt->label, N_("Catalog Tab"));
  mt->draw = catalog_tab_context_menu_draw;
  WM_menutype_add(mt);
}

void catalog_tabs_drag_drop_register()
{
  WM_operatortype_append(ASSET_OT_shelf_catalog_reorder);
  catalog_tab_context_menu_register();

  ListBaseT<wmDropBox> *lb = WM_dropboxmap_find("User Interface", SPACE_EMPTY, RGN_TYPE_WINDOW);
  wmDropBox *drop = WM_dropbox_add(lb,
                                   "ASSET_OT_shelf_catalog_reorder",
                                   catalog_tab_reorder_drop_poll,
                                   catalog_tab_reorder_drop_copy,
                                   nullptr,
                                   catalog_tab_reorder_drop_tooltip);
  drop->draw_in_view = catalog_tab_reorder_draw_in_view;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset Shelf Header Region
 *
 * Implemented as HeaderType for #RGN_TYPE_ASSET_SHELF_HEADER.
 * \{ */

static void asset_shelf_header_draw(const bContext *C, Header *header)
{
  ui::Layout &layout = *header->layout;
  ui::Block *block = layout.block();
  const AssetLibraryReference *library_ref = CTX_wm_asset_library_ref(C);

  list::storage_fetch(library_ref, C);

  block_emboss_set(block, ui::EmbossType::None);
  layout.popover(C, "ASSETSHELF_PT_catalog_selector", "", ICON_COLLAPSEMENU);
  block_emboss_set(block, ui::EmbossType::Emboss);

  layout.separator();

  PointerRNA shelf_ptr = active_shelf_ptr_from_context(C);
  if (AssetShelf *shelf = static_cast<AssetShelf *>(shelf_ptr.data)) {
    add_catalog_tabs(*shelf, layout);
  }

  layout.separator_spacer();

  layout.popover(C, "ASSETSHELF_PT_display", "", ICON_IMGDISPLAY);
  ui::Layout &sub = layout.row(false);
  /* Same as file/asset browser header. */
  sub.ui_units_x_set(8);
  sub.prop(&shelf_ptr, "search_filter", UI_ITEM_NONE, "", ICON_VIEWZOOM);
}

static void header_regiontype_register(ARegionType *region_type, const int space_type)
{
  HeaderType *ht = MEM_new_zeroed<HeaderType>(__func__);
  STRNCPY_UTF8(ht->idname, "ASSETSHELF_HT_settings");
  ht->space_type = space_type;
  ht->region_type = RGN_TYPE_ASSET_SHELF_HEADER;
  ht->draw = asset_shelf_header_draw;
  ht->poll = [](const bContext *C, HeaderType *) {
    return asset_shelf_space_poll(C, CTX_wm_space_data(C));
  };

  BLI_addtail(&region_type->headertypes, ht);
}

void types_register(ARegionType *region_type, const int space_type)
{
  header_regiontype_register(region_type, space_type);
  catalog_selector_panel_register(region_type);
  popover_panel_register(region_type);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset Shelf Type (un)registration
 * \{ */

void type_unlink(const Main &bmain, const AssetShelfType &shelf_type)
{
  for (bScreen &screen : bmain.screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &sl : area.spacedata) {
        ListBaseT<ARegion> *regionbase = (&sl == area.spacedata.first) ? &area.regionbase :
                                                                         &sl.regionbase;
        for (ARegion &region : *regionbase) {
          if (region.regiontype != RGN_TYPE_ASSET_SHELF) {
            continue;
          }

          RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(
              region);
          if (!shelf_regiondata) {
            continue;
          }
          for (AssetShelf &shelf : shelf_regiondata->shelves) {
            if (shelf.type == &shelf_type) {
              shelf.type = nullptr;
            }
          }

          BLI_assert((shelf_regiondata->active_shelf == nullptr) ||
                     (shelf_regiondata->active_shelf->type != &shelf_type));
        }
      }
    }
  }

  type_popup_unlink(shelf_type);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name External helpers
 * \{ */

void show_catalog_in_visible_shelves(const bContext &C, const StringRefNull catalog_path)
{
  wmWindowManager *wm = CTX_wm_manager(&C);
  for (wmWindow &win : wm->windows) {
    const bScreen *screen = WM_window_get_active_screen(&win);
    for (ScrArea &area : screen->areabase) {
      if (AssetShelf *shelf = asset::shelf::active_shelf_from_area(&area)) {
        settings_set_catalog_path_enabled(*shelf, catalog_path.c_str());
      }
    }
  }
}

/** \} */

}  // namespace blender::ed::asset::shelf
