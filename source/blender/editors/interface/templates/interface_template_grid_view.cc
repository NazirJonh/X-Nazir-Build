/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Generic grid renderer: tiles + smooth scroll + resize grip + overlay scrollbar.
 * Knows nothing about View3D / assets / images. All UI state is accessed via #GridStateAccess
 * and all windowed math via interface_grid_view.hh.
 */

#include "interface_grid_view.hh"
#include "interface_grid_view_settings_utils.hh"
#include "interface_grid_view_sources.hh"
#include "interface_intern.hh"

#include "DNA_view2d_types.h"

#include "BLI_math_base.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_sys_types.h"
#include "BLI_utildefines.h"

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

void grid_view_fill_from_source(AbstractGridView &view,
                                GridDataSource &source,
                                const bContext &C,
                                GridSessionState *session,
                                const int cols,
                                const GridViewHostParams &params)
{
  const int tile_h = max_ii(view.get_style().tile_height, 1);
  const int safe_cols = max_ii(1, cols);
  const int grip = session ? session->grip_pixel_height : 0;
  const int item_window = grid_build_window_size(
      grip, tile_h, safe_cols, params.max_items, params.max_rows);
  const int first_index = session ? (session->scroll_px / tile_h) * safe_cols : 0;
  const int count = source.build_window_and_count(C, view, IndexRange(first_index, item_window));
  if (session && source.item_count_ready(C)) {
    session->cached_item_count = count;
  }
}

namespace {

/* -------------------------------------------------------------------- */
/** \name Generic grid view
 * \{ */

/**
 * Per-redraw view over a #GridSessionState registry entry (see `interface_grid_view.hh`).
 * #GenericGridView itself is rebuilt from scratch on every redraw like all views, so grip
 * height / scroll position / item count live in the grid_id-keyed session instead. #uiViewState
 * (#AbstractView::persistent_state()) doesn't work for this: it only round-trips through
 * #block_views_end() / #block_view_persistent_state_restore(), which save/restore at the
 * start/end of building a block's layout — not while a #ButtonType::Grip / #ButtonType::Scroll
 * button is live-dragging the bound pointer. Keeping the state in a registry keyed by grid_id
 * sidesteps the save/restore round-trip entirely: the drag and the next redraw's read both go
 * through the same storage.
 *
 * Keyed by `grid_id`: two draw sites passing the *same* grid_id deliberately share one
 * scroll/grip state, so callers wanting independent grids must pass distinct, globally-unique
 * grid_id strings (spelled out in the docstrings of #template_grid_view_asset /
 * #template_grid_view_custom).
 */
class GenericGridView : public AbstractGridView {
  const bContext &context_;
  std::unique_ptr<GridDataSource> source_;
  int cols_hint_ = 1;
  GridViewHostParams host_params_;

 public:
  GenericGridView(const bContext &context,
                  std::unique_ptr<GridDataSource> source,
                  const int cols_hint,
                  const StringRef grid_id)
      : context_(context), source_(std::move(source)), cols_hint_(max_ii(1, cols_hint))
  {
    /* Attach to (acquire) the shared session; the base destructor releases it when
     * #block_free_views drops the owning #ViewLink — the teardown signal the session
     * refcount is keyed on. */
    this->use_session_scroll(grid_id);
  }

  GridSessionState &session()
  {
    BLI_assert(session_ != nullptr);
    return *session_;
  }

  int grip_pixel_height() const
  {
    return session_->grip_pixel_height;
  }
  void grip_pixel_height_set(const int value)
  {
    session_->grip_pixel_height = value;
  }
  int &grip_pixel_height_mut()
  {
    return session_->grip_pixel_height;
  }

  int host_max_rows() const
  {
    return host_params_.max_rows;
  }

  int cached_item_count() const
  {
    return session_->cached_item_count;
  }

  void build_items() override
  {
    grid_view_fill_from_source(
        *this, *source_, context_, session_, cols_hint_, host_params_);
  }

  int get_cached_item_count_for_build() const
  {
    return session_->cached_item_count;
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

  int scroll_px() const override
  {
    return view_.session().scroll_px;
  }
  void scroll_px_set(const int value) override
  {
    view_.session().scroll_px = value;
  }
  int *scroll_px_ptr() override
  {
    /* Pixel-scale scrollbar binds directly to the session's pixel scroll position. */
    return &view_.session().scroll_px;
  }

  int cached_item_count() const override
  {
    return view_.get_cached_item_count_for_build();
  }

  int cached_cols() const override
  {
    return view_.cols_per_row();
  }
  void cached_cols_set(const int /*value*/) override {}

  void store_scroll_for_cols(const int cols) override
  {
    GridSessionState &session = view_.session();
    session.scroll_px_by_cols.add_overwrite(cols, session.scroll_px);
  }

  void geometry_store(const ARegion *region,
                      const int tile_h,
                      const int cols,
                      const int viewport_px) override
  {
    GridSessionState &session = view_.session();
    /* Column count changed (width or preview size): restore the scroll pinned for the new
     * layout, so switching back and forth keeps each layout's position. Takes effect on the
     * next build; the current frame was already windowed with the old position. */
    if (session.cols != 0 && session.cols != cols) {
      if (const int *pinned = session.scroll_px_by_cols.lookup_ptr(cols)) {
        session.scroll_px = *pinned;
      }
    }
    session.tile_h = tile_h;
    session.cols = cols;
    session.viewport_px = viewport_px;
    session.region = region;
  }

  int effective_rows_dna_fallback() const override
  {
    const int tile_h = max_ii(1, view_.get_style().tile_height);
    return clamp_i(
        int(divide_ceil_u(uint(max_ii(view_.grip_pixel_height(), tile_h)), uint(tile_h))),
        1,
        view_.host_max_rows());
  }

  std::function<void(bContext &)> make_scroll_widget_fn(const int /*store_cols*/,
                                                        const int /*store_rows*/) const override
  {
    /* Pixel-scale widget already wrote the new #scroll_px directly; just redraw. */
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
      if (ELEM(wmn->data,
               int(ND_ASSET_LIST),
               int(ND_ASSET_LIST_READING),
               int(ND_ASSET_LIST_PREVIEW)))
      {
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

}  // namespace

void build_grid_view(const bContext &C,
                     Layout &layout,
                     AbstractGridView &view,
                     GridStateAccess &state,
                     const int item_count,
                     const int cols_est,
                     const int panel_width,
                     const GridViewHostParams &host)
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

  /* Clamp the raw grip to the 1..max_rows range for display; preserve the raw value so a temporary
   * preview-size change does not permanently shrink a height the user set at a smaller tile. */
  const int visible_height = clamp_i(
      state.grip_pixel_height(), tile_h, host.max_rows * tile_h);
  const int effective_rows = clamp_i(
      int(divide_ceil_u(uint(visible_height), uint(tile_h))), 1, host.max_rows);

  /* Total rows from the previous-frame item count (updated inside build_items this frame).
   * Falls back to effective_rows when the grid is empty so the grip does not collapse. */
  const int total_rows = grid_total_rows(item_count, cols_est, effective_rows);
  /* Pixel-exact scroll range: the whole content minus the raw pixel viewport, so the last scroll
   * position pulls a partial bottom row fully into view instead of quantizing to whole rows. The
   * sub-row remainder on the last page emerges naturally from this clamp.
   *
   * Skipped when #item_count is 0: that means #build_items() has never run for this session yet
   * (freshly created #GridStateAccess, e.g. a popover's first-ever build), so #item_count is not
   * "empty content" but "unknown". Clamping against a false 0 here would wipe out a same-frame
   * scroll request (e.g. #image_grid_request_scroll_to_asset) before #build_items() below gets a
   * chance to read it. The post-build clamp further down re-clamps against the real count once
   * #build_items() has run, so real overflow still gets bounded correctly. */
  if (item_count > 0) {
    const int max_scroll = grid_max_scroll_px(item_count, cols_est, tile_h, visible_height);
    state.scroll_px_set(grid_clamp_scroll_px(state.scroll_px(), max_scroll));
  }
  const int scroll_offset_px = state.scroll_px() % tile_h;

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

  /* Rows to build for the clipped window: every row it intersects at the current sub-row offset
   * (see #grid_rows_to_build — the single formula shared with the fixed-viewport host). Rows
   * beyond the window edge are drawn clipped instead of vanishing (most visible during
   * touch/drag scroll). */
  const int rows_to_build = grid_rows_to_build(visible_height, tile_h, scroll_offset_px);

  View2D local_v2d{};
  local_v2d.flag |= V2D_IS_INIT;
  local_v2d.tot.xmin = 0.0f;
  local_v2d.tot.xmax = float(grid_width);
  local_v2d.tot.ymin = float(-total_height);
  local_v2d.tot.ymax = 0.0f;
  local_v2d.cur.xmin = 0.0f;
  local_v2d.cur.xmax = float(grid_width);
  /* Keep cur at top of content so BuildOnlyVisibleButtonsHelper starts from item_idx 0.
   * The windowed build_items already offsets by the scroll row; re-offsetting here would
   * cause a double-skip and show the wrong items. */
  const int build_height = rows_to_build * tile_h;
  local_v2d.cur.ymin = float(-build_height);
  local_v2d.cur.ymax = 0.0f;
  BLI_rcti_init(&local_v2d.mask, 0, grid_width, -build_height, 0);

  Layout &grid_view_col = grid_stack.column(true);
  grid_view_col.fixed_size_set(true);
  if (panel_width > 0) {
    grid_view_col.ui_units_x_set(float(panel_width) / float(UI_UNIT_X));
  }
  grid_view_col.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));
  grid_view_col.view_scroll_clip_set(visible_height, scroll_offset_px, &view);

  GridViewBuilder builder(*block);
  builder.build_grid_view(C, view, grid_view_col, "", &local_v2d);

  /* Update cached cols from the actual column count reported by the view after build. */
  const int actual_cols = view.cols_per_row();
  state.cached_cols_set(actual_cols);

  /* #item_count is last frame's count, used above before #build_items() (inside
   * #builder.build_grid_view()) re-affirmed the real, current count. Using the stale parameter
   * here instead of #state.cached_item_count() would under/over-count on any frame where the
   * item count actually changed, most visibly as a scrollbar that never appears because the
   * stale count it starts from (0, for a freshly built view whose GridStateAccess has no prior
   * frame to read from, e.g. #ViewGridStateAccess) never reflects real content. */
  const int max_scroll_post = grid_max_scroll_px(
      state.cached_item_count(), actual_cols, tile_h, visible_height);
  state.scroll_px_set(grid_clamp_scroll_px(state.scroll_px(), max_scroll_post));
  state.store_scroll_for_cols(actual_cols);

  /* Geometry snapshot for the input layer (session-backed states): stable across rebuilds, so
   * drags and hit-tests never depend on the tiles built this frame. */
  {
    const ARegion *region = CTX_wm_region_popup(&C) ? CTX_wm_region_popup(&C) :
                                                      CTX_wm_region(&C);
    if (block->handle != nullptr && block->handle->region != nullptr) {
      region = block->handle->region;
    }
    state.geometry_store(region, tile_h, actual_cols, visible_height);
  }

  /* --- Overlay scrollbar (does not steal grid width) --- */

  if (max_scroll_post > 0) {
    Layout &scroll_anchor = grid_stack.row(false);
    scroll_anchor.alignment_set(LayoutAlign::Right);
    Layout &scroll_col = scroll_anchor.column(false);
    scroll_col.fixed_size_set(true);
    scroll_col.ui_units_x_set(float(V2D_SCROLL_WIDTH) / float(UI_UNIT_X));
    scroll_col.ui_units_y_set(float(visible_height) / float(UI_UNIT_Y));

    /* Pixel-scale scrollbar bound directly to #GridSessionState::scroll_px: value range is the
     * pixel scroll range and #visual_height the pixel viewport, so the thumb sizes to a partial
     * bottom row and the drag glides smoothly instead of quantizing to whole rows. */
    block_layout_set_current(block, &scroll_col);
    Button *but = uiDefButV(block,
                            ButtonType::Scroll,
                            "",
                            0,
                            0,
                            short(V2D_SCROLL_WIDTH),
                            visible_height,
                            state.scroll_px_ptr(),
                            0.0f,
                            float(max_scroll_post),
                            "");
    auto *but_scroll = reinterpret_cast<ButtonScrollBar *>(but);
    but_scroll->visual_height = float(visible_height);
    uchar scroll_track_bg[4];
    theme::get_color_4ubv(TH_BACK, scroll_track_bg);
    scroll_track_bg[3] = 255;
    button_color_set(but, scroll_track_bg);
    button_flag_disable(but, BUT_UNDO);
    button_func_set(but, state.make_scroll_widget_fn(actual_cols, effective_rows));
    block_layout_set_current(block, &layout);
  }

  /* --- Resize grip --- */

  if (host.show_grip) {
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
}

void template_grid_view_asset(Layout *layout,
                              bContext *C,
                              const char *grid_id,
                              PointerRNA *settings_ptr,
                              const char *activate_operator,
                              const char *drag_operator)
{
  if (!layout || !C || !grid_id || !grid_id[0] || !settings_ptr || !settings_ptr->data) {
    return;
  }

  Block *block = layout->block();

  const AssetLibraryReference lib_ref = grid_settings::library_ref_get(*settings_ptr);
  Set<std::string> catalogs = grid_settings::enabled_catalogs_get(*settings_ptr);
  Set<short> filter_id_types = grid_settings::filter_id_types_get(*settings_ptr);
  NameMatchFilterState name_match = grid_settings::name_match_filter_get(*settings_ptr);
  const int preview_size = grid_settings::preview_size_get(*settings_ptr);

  const int tile_w = preview_tile_size_x(preview_size);
  const int tile_h = preview_tile_size_y_no_label(preview_size);
  const int panel_width = max_ii(layout->width(), 0);
  const int cols_est = (panel_width > 0) ? max_ii(1, panel_width / max_ii(tile_w, 1)) : 1;

  auto source = std::make_unique<AssetGridDataSource>(lib_ref,
                                                      std::move(catalogs),
                                                      std::move(filter_id_types),
                                                      std::move(name_match),
                                                      activate_operator ? activate_operator : "",
                                                      drag_operator ? drag_operator : "");

  auto view_unique = std::make_unique<GenericGridView>(*C, std::move(source), cols_est, grid_id);
  GenericGridView *view_ptr = view_unique.get();
  view_unique->set_tile_size(tile_w, tile_h);
  view_unique->set_cols_per_row_hint(cols_est);

  AbstractGridView *grid_view = block_add_view(*block, grid_id, std::move(view_unique));

  block_add_dynamic_listener(block, ed::asset::list::asset_reading_region_listen_fn);
  block_add_dynamic_listener(block, grid_view_block_listener);

  ViewGridStateAccess state_access(*view_ptr, grid_id);
  build_grid_view(*C,
                  *layout,
                  *grid_view,
                  state_access,
                  view_ptr->get_cached_item_count_for_build(),
                  cols_est,
                  panel_width);
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
    RNA_warning(
        "Expected a collection property: %s.%s", RNA_struct_identifier(dataptr->type), propname);
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

  auto view_unique = std::make_unique<GenericGridView>(*C, std::move(source), cols_est, grid_id);
  GenericGridView *view_ptr = view_unique.get();
  view_unique->set_tile_size(tile_w, tile_h);
  view_unique->set_cols_per_row_hint(cols_est);

  AbstractGridView *grid_view = block_add_view(*block, grid_id, std::move(view_unique));

  ViewGridStateAccess state_access(*view_ptr, grid_id);
  build_grid_view(*C,
                  *layout,
                  *grid_view,
                  state_access,
                  view_ptr->get_cached_item_count_for_build(),
                  cols_est,
                  panel_width);
}

} /* namespace blender::ui */
