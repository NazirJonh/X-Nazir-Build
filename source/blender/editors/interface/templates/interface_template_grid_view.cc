/* SPDX-FileCopyrightText: 2026 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Generic grid renderer: tiles + smooth scroll + resize grip + overlay scrollbar.
 * Knows nothing about View3D / assets / images. All UI state is accessed via #GridStateAccess
 * and all windowed math via interface_grid_view.hh.
 */

#include "interface_grid_view.hh"
#include "interface_grid_view_sources.hh"
#include "interface_grid_view_settings_utils.hh"
#include "interface_intern.hh"

#include "DNA_view2d_types.h"

#include "BLI_math_base.h"
#include "BLI_rect.h"
#include "BLI_string.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "ED_asset_list.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ui {

namespace {

constexpr int GRID_MAX_ITEMS = 512;

/* -------------------------------------------------------------------- */
/** \name Generic grid view + session state (uiViewState-backed)
 * \{ */

class GenericGridView : public AbstractGridView {
  const bContext &context_;
  std::unique_ptr<GridDataSource> source_;
  int cols_hint_ = 1;

  /* Session UI state — persisted via #persistent_state() / #uiViewState. */
  int grip_pixel_height_ = 0;
  int scroll_row_ = 0;
  int scroll_offset_px_ = 0;
  int cached_item_count_ = 0;

 public:
  GenericGridView(const bContext &context,
                  std::unique_ptr<GridDataSource> source,
                  const int cols_hint)
      : context_(context), source_(std::move(source)), cols_hint_(max_ii(1, cols_hint))
  {
  }

  int grip_pixel_height() const
  {
    return grip_pixel_height_;
  }
  void grip_pixel_height_set(const int value)
  {
    grip_pixel_height_ = value;
  }
  int &grip_pixel_height_mut()
  {
    return grip_pixel_height_;
  }

  int scroll_row() const
  {
    return scroll_row_;
  }
  void scroll_row_set(const int value)
  {
    scroll_row_ = value;
  }
  int &scroll_row_mut()
  {
    return scroll_row_;
  }

  int scroll_offset_px() const
  {
    return scroll_offset_px_;
  }
  void scroll_offset_px_set(const int value)
  {
    scroll_offset_px_ = value;
  }

  int cached_item_count() const
  {
    return cached_item_count_;
  }

  std::optional<uiViewState> persistent_state() const override
  {
    uiViewState state{};
    if (grip_pixel_height_ > 0) {
      state.custom_height = int(round_fl_to_int(float(grip_pixel_height_) * UI_INV_SCALE_FAC));
    }
    state.scroll_offset = scroll_row_;
    return state;
  }

  void persistent_state_apply(const uiViewState &state) override
  {
    if (state.custom_height > 0) {
      grip_pixel_height_ = int(state.custom_height * UI_SCALE_FAC);
    }
    scroll_row_ = state.scroll_offset;
  }

  void build_items() override
  {
    const int cols = cols_hint_;
    const int tile_h = max_ii(1, get_style().tile_height);
    const int item_window = grid_build_window_size(grip_pixel_height_, tile_h, cols, GRID_MAX_ITEMS);
    const int first_index = scroll_row_ * cols;
    const IndexRange window(first_index, item_window);

    cached_item_count_ = source_->item_count(context_);
    source_->build_window(context_, *this, window);
  }

  int get_cached_item_count_for_build() const
  {
    return cached_item_count_;
  }
};

/** Forwards #GridStateAccess to a #GenericGridView's session members. */
class ViewGridStateAccess : public GridStateAccess {
  GenericGridView &view_;
  std::string idname_;

 public:
  ViewGridStateAccess(GenericGridView &view, std::string idname)
      : view_(view), idname_(std::move(idname))
  {
  }

  int grip_pixel_height() const override
  {
    return view_.grip_pixel_height();
  }
  void grip_pixel_height_set(const int value) override
  {
    view_.grip_pixel_height_set(value);
  }
  int *grip_pixel_height_ptr() override
  {
    return &view_.grip_pixel_height_mut();
  }

  int scroll_row() const override
  {
    return view_.scroll_row();
  }
  void scroll_row_set(const int value) override
  {
    view_.scroll_row_set(value);
  }
  int *scroll_row_ptr() override
  {
    return &view_.scroll_row_mut();
  }

  int scroll_offset_px() const override
  {
    return view_.scroll_offset_px();
  }
  void scroll_offset_px_set(const int value) override
  {
    view_.scroll_offset_px_set(value);
  }

  int cached_item_count() const override
  {
    return view_.get_cached_item_count_for_build();
  }
  void cached_item_count_set(const int /*value*/) override {}

  int cached_cols() const override
  {
    return view_.cols_per_row();
  }
  void cached_cols_set(const int /*value*/) override {}

  void store_scroll_for_layout(const int /*cols*/, const int /*rows*/) override {}
  void focus_clear() override {}

  int effective_rows_dna_fallback() const override
  {
    const int tile_h = max_ii(1, view_.get_style().tile_height);
    return clamp_i(int(divide_ceil_u(uint(max_ii(view_.grip_pixel_height(), tile_h)),
                                     uint(tile_h))),
                   1,
                   16);
  }

  std::function<void(bContext &)> make_scroll_widget_fn(const int /*store_cols*/,
                                                        const int /*store_rows*/) const override
  {
    return [](bContext &C) {
      if (ARegion *region = CTX_wm_region(&C)) {
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }
    };
  }

  std::function<void(bContext &)> make_grip_change_fn() const override
  {
    return [](bContext &C) {
      if (ARegion *region = CTX_wm_region(&C)) {
        ED_region_tag_redraw(region);
        ED_region_tag_refresh_ui(region);
      }
    };
  }

  StringRef grid_idname() const override
  {
    return idname_;
  }
};

static void grid_view_block_listener(const wmRegionListenerParams *params)
{
  const wmNotifier *wmn = params->notifier;
  switch (wmn->category) {
    case NC_ASSET:
      if (ELEM(wmn->data, int(ND_ASSET_LIST), int(ND_ASSET_LIST_READING), int(ND_ASSET_LIST_PREVIEW))) {
        ED_region_tag_redraw(params->region);
        if (wmn->data != int(ND_ASSET_LIST_PREVIEW)) {
          ED_region_tag_refresh_ui(params->region);
        }
      }
      break;
    default:
      break;
  }
}

/** \} */

}  // namespace

void build_grid_view(const bContext &C,
                     Layout &layout,
                     AbstractGridView &view,
                     GridStateAccess &state,
                     const int item_count,
                     const int cols_est,
                     const int panel_width)
{
  Block *block = layout.block();

  const GridViewStyle &style = view.get_style();
  const int tile_h = style.tile_height;

  /* Reconstruct grip height from the DNA row count on the first frame (grip == 0 when unset) or
   * after file reload. This is the only place that reads the fallback so the core stays neutral
   * about storage. */
  if (state.grip_pixel_height() < tile_h) {
    state.grip_pixel_height_set(state.effective_rows_dna_fallback() * tile_h);
  }

  /* Clamp the raw grip to the 1..16-row range for display; preserve the raw value so a temporary
   * preview-size change does not permanently shrink a height the user set at a smaller tile. */
  const int visible_height = clamp_i(state.grip_pixel_height(), tile_h, 16 * tile_h);
  const int effective_rows = clamp_i(
      int(divide_ceil_u(uint(visible_height), uint(tile_h))), 1, 16);

  /* Compute total rows from the previous-frame item count (updated inside build_items this frame).
   * Falls back to effective_rows when the grid is empty so the grip does not collapse. */
  const int total_rows = grid_total_rows(item_count, cols_est, effective_rows);
  const int max_scroll = grid_clamp_scroll_row(
      max_ii(0, total_rows - effective_rows), max_ii(0, total_rows - effective_rows));
  state.scroll_row_set(grid_clamp_scroll_row(state.scroll_row(), max_scroll));

  /* Sub-row offset: pin to a whole-row boundary on the last row (nothing below to reveal). */
  if (state.scroll_row() >= max_scroll) {
    state.scroll_offset_px_set(0);
  }
  else {
    state.scroll_offset_px_set(clamp_i(state.scroll_offset_px(), 0, tile_h - 1));
  }
  const int scroll_offset_px = state.scroll_offset_px();

  /* --- Layout construction --- */

  Layout &outer_row = layout.row(false);
  Layout &grid_layout = outer_row.column(true);
  grid_layout.fixed_size_set(true);
  if (panel_width > 0) {
    grid_layout.ui_units_x_set(float(panel_width) / float(UI_UNIT_X));
  }
  grid_layout.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));

  Layout &grid_stack = grid_layout.overlap();
  grid_stack.fixed_size_set(true);
  if (panel_width > 0) {
    grid_stack.ui_units_x_set(float(panel_width) / float(UI_UNIT_X));
  }
  grid_stack.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));

  const int grid_width = (panel_width > 0) ? panel_width : max_ii(style.tile_width, 1);
  const int total_height = max_ii(visible_height, total_rows * tile_h);

  /* Rows to mark visible (and build) beyond the clipped window. One buffer row covers a window
   * height that is not an exact multiple of #tile_h. A sub-row #scroll_offset_px shifts content up,
   * so the window then intersects one *more* row at the bottom; without a second buffer row that
   * partial bottom row falls outside #BuildOnlyVisibleButtonsHelper's range and vanishes entirely
   * instead of being drawn clipped (most visible during touch/drag scroll). */
  const int buffer_rows = (scroll_offset_px > 0) ? 2 : 1;

  View2D local_v2d{};
  local_v2d.flag |= V2D_IS_INIT;
  local_v2d.tot.xmin = 0.0f;
  local_v2d.tot.xmax = float(grid_width);
  local_v2d.tot.ymin = float(-total_height);
  local_v2d.tot.ymax = 0.0f;
  local_v2d.cur.xmin = 0.0f;
  local_v2d.cur.xmax = float(grid_width);
  /* Keep cur at top of content so BuildOnlyVisibleButtonsHelper starts from item_idx 0.
   * The windowed build_items already offsets by scroll_row*cols; re-offsetting here would
   * cause a double-skip and show the wrong items. */
  const int build_height = visible_height + buffer_rows * tile_h;
  local_v2d.cur.ymin = float(-build_height);
  local_v2d.cur.ymax = 0.0f;
  BLI_rcti_init(&local_v2d.mask, 0, grid_width, -build_height, 0);

  Layout &grid_view_col = grid_stack.column(true);
  grid_view_col.fixed_size_set(true);
  if (panel_width > 0) {
    grid_view_col.ui_units_x_set(float(panel_width) / float(UI_UNIT_X));
  }
  grid_view_col.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));
  grid_view_col.view_scroll_clip_set(visible_height, scroll_offset_px);

  GridViewBuilder builder(*block);
  builder.build_grid_view(C, view, grid_view_col, "", &local_v2d);

  /* Update cached cols from the actual column count reported by the view after build. */
  const int actual_cols = view.cols_per_row();
  state.cached_cols_set(actual_cols);

  const int total_rows_post = grid_total_rows(item_count, actual_cols, effective_rows);
  const int max_scroll_post = max_ii(0, total_rows_post - effective_rows);
  state.scroll_row_set(grid_clamp_scroll_row(state.scroll_row(), max_scroll_post));
  state.store_scroll_for_layout(actual_cols, effective_rows);

  /* --- Overlay scrollbar (does not steal grid width) --- */

  if (max_scroll_post > 0) {
    Layout &scroll_anchor = grid_stack.row(false);
    scroll_anchor.alignment_set(LayoutAlign::Right);
    Layout &scroll_col = scroll_anchor.column(false);
    scroll_col.fixed_size_set(true);
    scroll_col.ui_units_x_set(float(V2D_SCROLL_WIDTH) / float(UI_UNIT_X));
    scroll_col.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));

    block_layout_set_current(block, &scroll_col);
    Button *but = uiDefButV(block,
                            ButtonType::Scroll,
                            "",
                            0,
                            0,
                            short(V2D_SCROLL_WIDTH),
                            visible_height,
                            state.scroll_row_ptr(),
                            0.0f,
                            float(max_scroll_post),
                            "");
    auto *but_scroll = reinterpret_cast<ButtonScrollBar *>(but);
    but_scroll->visual_height = float(effective_rows);
    uchar scroll_track_bg[4];
    theme::get_color_4ubv(TH_BACK, scroll_track_bg);
    scroll_track_bg[3] = 255;
    button_color_set(but, scroll_track_bg);
    button_flag_disable(but, BUT_UNDO);
    button_func_set(but, state.make_scroll_widget_fn(actual_cols, effective_rows));
    block_layout_set_current(block, &layout);
  }

  /* --- Resize grip --- */

  Layout &grip_row = layout.row(false);
  grip_row.scale_x_set(1.0f);
  block_layout_set_current(block, &grip_row);
  Button *grip_but = uiDefIconButV(block,
                                   ButtonType::Grip,
                                   ICON_GRIP,
                                   0,
                                   0,
                                   short(max_ii(panel_width, int(UI_UNIT_X * 10))),
                                   short(UI_UNIT_Y * 0.5f),
                                   state.grip_pixel_height_ptr(),
                                   0.0f,
                                   0.0f,
                                   "");
  button_flag_disable(grip_but, BUT_UNDO);
  button_func_set(grip_but, state.make_grip_change_fn());
  block_layout_set_current(block, &layout);
}

void template_grid_view_asset(Layout *layout,
                              bContext *C,
                              const char *grid_id,
                              PointerRNA *settings_ptr,
                              const char *activate_operator,
                              const char * /*drag_operator*/)
{
  if (!layout || !C || !grid_id || !grid_id[0] || !settings_ptr || !settings_ptr->data) {
    return;
  }

  Block *block = layout->block();

  const AssetLibraryReference lib_ref = grid_settings::library_ref_get(*settings_ptr);
  Set<std::string> catalogs = grid_settings::enabled_catalogs_get(*settings_ptr);
  const int preview_size = grid_settings::preview_size_get(*settings_ptr);

  const int tile_w = preview_tile_size_x(preview_size);
  const int tile_h = preview_tile_size_y_no_label(preview_size);
  const int panel_width = max_ii(layout->width(), 0);
  const int cols_est = (panel_width > 0) ? max_ii(1, panel_width / max_ii(tile_w, 1)) : 1;

  auto source = std::make_unique<AssetGridDataSource>(
      lib_ref,
      std::move(catalogs),
      activate_operator ? activate_operator : "");

  auto view_unique = std::make_unique<GenericGridView>(*C, std::move(source), cols_est);
  GenericGridView *view_ptr = view_unique.get();
  view_unique->set_tile_size(tile_w, tile_h);
  view_unique->set_cols_per_row_hint(cols_est);

  AbstractGridView *grid_view = block_add_view(*block, grid_id, std::move(view_unique));

  block_add_dynamic_listener(block, ed::asset::list::asset_reading_region_listen_fn);
  block_add_dynamic_listener(block, grid_view_block_listener);

  ViewGridStateAccess state_access(*view_ptr, grid_id);
  build_grid_view(
      *C, *layout, *grid_view, state_access, view_ptr->get_cached_item_count_for_build(), cols_est, panel_width);
}

void template_grid_view_custom(Layout *layout,
                               bContext *C,
                               const char *grid_id,
                               const char *gridtype_name,
                               PointerRNA *dataptr,
                               const char *propname,
                               PointerRNA *settings_ptr)
{
  if (!layout || !C || !grid_id || !grid_id[0] || !gridtype_name || !gridtype_name[0] ||
      !dataptr || !dataptr->data || !propname || !propname[0])
  {
    return;
  }

  uiGridType *grid_type = WM_uigridtype_find(gridtype_name, false);
  if (!grid_type) {
    RNA_warning("Grid type %s not found", gridtype_name);
    return;
  }

  PropertyRNA *prop = RNA_struct_find_property(dataptr, propname);
  if (!prop || RNA_property_type(prop) != PROP_COLLECTION) {
    RNA_warning("Expected a collection property: %s.%s",
                RNA_struct_identifier(dataptr->type),
                propname);
    return;
  }

  Block *block = layout->block();

  const int preview_size = (settings_ptr && settings_ptr->data) ?
                               grid_settings::preview_size_get(*settings_ptr) :
                               96;
  const int tile_w = preview_tile_size_x(preview_size);
  const int tile_h = preview_tile_size_y_no_label(preview_size);
  const int panel_width = max_ii(layout->width(), 0);
  const int cols_est = (panel_width > 0) ? max_ii(1, panel_width / max_ii(tile_w, 1)) : 1;

  auto source = std::make_unique<PyCallbackGridDataSource>(grid_type, *dataptr, propname);

  auto view_unique = std::make_unique<GenericGridView>(*C, std::move(source), cols_est);
  GenericGridView *view_ptr = view_unique.get();
  view_unique->set_tile_size(tile_w, tile_h);
  view_unique->set_cols_per_row_hint(cols_est);

  AbstractGridView *grid_view = block_add_view(*block, grid_id, std::move(view_unique));

  ViewGridStateAccess state_access(*view_ptr, grid_id);
  build_grid_view(
      *C, *layout, *grid_view, state_access, view_ptr->get_cached_item_count_for_build(), cols_est, panel_width);
}

}  /* namespace blender::ui */
