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

#include "BLT_translation.hh"

#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "AS_asset_representation.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"

#include "ED_asset_list.hh"
#include "ED_asset_library.hh"
#include "ED_image_grid.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include <fmt/format.h>

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
 * Keyed by `grid_id` within a region for Python templates (see #py_grid_session_id).
 * Two draw sites in the same region passing the same grid_id share scroll, like #uiList
 * `list_id`. Different regions do not. #template_grid_view_asset uses a region-scoped key;
 * the image-grid product uses its own owner-based session id.
 */
class GenericGridView : public AbstractGridView {
  const bContext &context_;
  std::unique_ptr<GridDataSource> source_;
  int cols_hint_ = 1;
  GridViewHostParams host_params_;
  uiGridType *grid_type_ = nullptr;

 public:
  GenericGridView(const bContext &context,
                  std::unique_ptr<GridDataSource> source,
                  const int cols_hint,
                  const StringRef grid_id,
                  uiGridType *grid_type = nullptr)
      : context_(context),
        source_(std::move(source)),
        cols_hint_(max_ii(1, cols_hint)),
        grid_type_(grid_type)
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

  bool listen(const wmNotifier &notifier) const override
  {
    if (!grid_type_ || !grid_type_->listen) {
      return false;
    }
    uiGrid grid_inst;
    grid_inst.type = grid_type_;
    return grid_type_->listen(&grid_inst, &notifier);
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

namespace {

/** Pixel geometry the rest of #build_grid_view lays out against. */
struct GridViewMetrics {
  int tile_h = 1;
  /** Grip height clamped to 1..max_rows tiles: the pixel viewport the grid draws into. */
  int visible_height = 0;
  /** #visible_height in whole tiles, for the widgets that still count in rows. */
  int effective_rows = 1;
  /** Content height in rows, from the previous frame's item count. */
  int total_rows = 1;
  /** Sub-tile remainder of the scroll position, so a partial top row is drawn clipped. */
  int scroll_offset_px = 0;
};

}  // namespace

/**
 * Resolve the grip height into pixel geometry, and pre-clamp the scroll position against the
 * previous frame's \a item_count. Writes to \a state: the first-frame grip fallback and that
 * clamp are both corrections the rest of the build depends on.
 */
static GridViewMetrics grid_view_resolve_metrics(GridStateAccess &state,
                                                 const int tile_h,
                                                 const int item_count,
                                                 const int cols_est,
                                                 const GridViewHostParams &host)
{
  /* Reconstruct grip height from the DNA row count on the first frame (grip == 0 when unset) or
   * after file reload. This is the only place that reads the fallback so the core stays neutral
   * about storage. */
  if (state.grip_pixel_height() < tile_h) {
    state.grip_pixel_height_set(state.effective_rows_dna_fallback() * tile_h);
  }

  GridViewMetrics metrics;
  metrics.tile_h = tile_h;
  /* Clamp the raw grip to the 1..max_rows range for display; preserve the raw value so a temporary
   * preview-size change does not permanently shrink a height the user set at a smaller tile. */
  metrics.visible_height = clamp_i(state.grip_pixel_height(), tile_h, host.max_rows * tile_h);
  metrics.effective_rows = clamp_i(
      int(divide_ceil_u(uint(metrics.visible_height), uint(tile_h))), 1, host.max_rows);

  /* Total rows from the previous-frame item count (updated inside build_items this frame).
   * Falls back to effective_rows when the grid is empty so the grip does not collapse. */
  metrics.total_rows = grid_total_rows(item_count, cols_est, metrics.effective_rows);

  /* Pixel-exact scroll range: the whole content minus the raw pixel viewport, so the last scroll
   * position pulls a partial bottom row fully into view instead of quantizing to whole rows. The
   * sub-row remainder on the last page emerges naturally from this clamp.
   *
   * Skipped when #item_count is 0: that means #build_items() has never run for this session yet
   * (freshly created #GridStateAccess, e.g. a popover's first-ever build), so #item_count is not
   * "empty content" but "unknown". Clamping against a false 0 here would wipe out a same-frame
   * scroll request (e.g. #image_grid_request_scroll_to_asset) before #build_items() gets a chance
   * to read it. The post-build clamp in #build_grid_view re-clamps against the real count once
   * #build_items() has run, so real overflow still gets bounded correctly. */
  if (item_count > 0) {
    const int max_scroll = grid_max_scroll_px(
        item_count, cols_est, tile_h, metrics.visible_height);
    state.scroll_px_set(grid_clamp_scroll_px(state.scroll_px(), max_scroll));
  }
  metrics.scroll_offset_px = state.scroll_px() % tile_h;
  return metrics;
}

/**
 * Scrollbar drawn on top of the right edge of \a grid_stack rather than beside it, so turning it
 * on does not reflow the tiles. \a layout is restored as the block's current layout afterwards.
 */
static void grid_view_add_overlay_scrollbar(Layout &layout,
                                            Layout &grid_stack,
                                            GridStateAccess &state,
                                            const int visible_height,
                                            const int actual_cols,
                                            const int effective_rows,
                                            const int max_scroll_px)
{
  Block *block = layout.block();

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
                          float(max_scroll_px),
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

namespace {

/**
 * How the filter row beside the grip is backed. A host either has RNA #GridViewSettings, or plain
 * memory when its state is not RNA (the image grid keeps its state in DNA plus a runtime struct).
 * Same widgets either way, so the row reads identically; only the binding differs.
 */
struct GridViewFilterBacking {
  bool use_rna = false;
  bool use_raw = false;
  /** Whether the search field below the grip is currently disclosed. */
  bool disclosed = false;

  bool present() const
  {
    return use_rna || use_raw;
  }
};

}  // namespace

static GridViewFilterBacking grid_view_filter_backing(const GridViewHostParams &host)
{
  GridViewFilterBacking backing;
  backing.use_rna = host.filter_settings != nullptr && host.filter_settings->data != nullptr;
  backing.use_raw = !backing.use_rna && host.filter_show != nullptr &&
                    host.filter_search_buf != nullptr && host.filter_search_maxncpy > 0;
  backing.disclosed = backing.use_rna ? grid_settings::show_filter_get(*host.filter_settings) :
                                        (backing.use_raw && *host.filter_show);
  return backing;
}

/**
 * Disclosure triangle for the filter row, sharing the grip's line exactly like #uiList's (see the
 * #uilist_draw_filter row in `interface_template_list.cc`). Drawn embossless so it reads as chrome.
 */
static void grid_view_add_filter_disclosure_button(Block &block,
                                                   const GridViewHostParams &host,
                                                   const GridViewFilterBacking &backing)
{
  const int icon = backing.disclosed ? ICON_DISCLOSURE_TRI_DOWN : ICON_DISCLOSURE_TRI_RIGHT;
  const StringRef tip = backing.disclosed ? TIP_("Hide filtering options") :
                                            TIP_("Show filtering options");

  const EmbossType previous_emboss = block_emboss_get(&block);
  block_emboss_set(&block, EmbossType::None);
  Button *but = backing.use_rna ? uiDefIconButR(&block,
                                                ButtonType::Toggle,
                                                icon,
                                                0,
                                                0,
                                                short(UI_UNIT_X),
                                                short(UI_UNIT_Y * 0.5f),
                                                host.filter_settings,
                                                "show_filter",
                                                -1,
                                                0.0f,
                                                0.0f,
                                                tip) :
                                  uiDefIconButV(&block,
                                                ButtonType::Toggle,
                                                icon,
                                                0,
                                                0,
                                                short(UI_UNIT_X),
                                                short(UI_UNIT_Y * 0.5f),
                                                host.filter_show,
                                                0.0f,
                                                0.0f,
                                                tip);
  /* Screen state, not data -- the same reason #uiList's toggle skips undo. */
  button_flag_disable(but, BUT_UNDO);
  block_emboss_set(&block, previous_emboss);
}

/** The disclosed search field, in the same shape as #uiList's own, placeholder included. */
static void grid_view_add_filter_search_row(Layout &layout,
                                            const GridViewHostParams &host,
                                            const GridViewFilterBacking &backing,
                                            const int panel_width)
{
  Block *block = layout.block();
  Layout &filter_row = layout.row(true);
  if (backing.use_rna) {
    filter_row.prop(host.filter_settings,
                    RNA_struct_find_property(host.filter_settings, "filter_search"),
                    -1,
                    0,
                    UI_ITEM_NONE,
                    "",
                    ICON_VIEWZOOM,
                    IFACE_("Search"));
  }
  else {
    block_layout_set_current(block, &filter_row);
    Button *search_but = uiDefBut(block,
                                  ButtonType::Text,
                                  "",
                                  0,
                                  0,
                                  short(max_ii(panel_width, int(UI_UNIT_X * 10))),
                                  short(UI_UNIT_Y),
                                  host.filter_search_buf,
                                  0.0f,
                                  float(host.filter_search_maxncpy),
                                  TIP_("Only show items whose name contains this text"));
    button_placeholder_set(search_but, IFACE_("Search"));
    button_flag_disable(search_but, BUT_UNDO);
  }
  block_layout_set_current(block, &layout);
}

/**
 * The resize grip under the grid, with the optional filter disclosure triangle on its line and
 * the search field below it. \a layout is restored as the block's current layout afterwards.
 */
static void grid_view_add_grip_row(Layout &layout,
                                   GridStateAccess &state,
                                   const int panel_width,
                                   const GridViewHostParams &host)
{
  Block *block = layout.block();
  const GridViewFilterBacking backing = grid_view_filter_backing(host);

  Layout &grip_row = layout.row(false);
  grip_row.scale_x_set(1.0f);
  block_layout_set_current(block, &grip_row);

  int grip_width = max_ii(panel_width, int(UI_UNIT_X * 10));
  if (backing.present()) {
    grid_view_add_filter_disclosure_button(*block, host, backing);
    /* Room for the triangle *and* for the matching spacer added after the grip below. The grip
     * draws #ICON_GRIP centered in its own rect, so without the spacer the triangle on one side
     * only would push that center off the panel's center. */
    grip_width = max_ii(grip_width - 2 * int(UI_UNIT_X), int(UI_UNIT_X));
  }

  Button *grip_but = uiDefIconButV(block,
                                   ButtonType::Grip,
                                   ICON_GRIP,
                                   0,
                                   0,
                                   short(grip_width),
                                   short(UI_UNIT_Y * 0.5f),
                                   state.grip_pixel_height_ptr(),
                                   0.0f,
                                   0.0f,
                                   "");
  button_flag_disable(grip_but, BUT_UNDO);
  button_func_set(grip_but, state.make_grip_change_fn());

  if (backing.present()) {
    /* Mirrors the disclosure triangle so the grip stays centered on the panel; a separator is
     * inert, so it cannot steal the drag the grip needs. */
    uiDefBut(block,
             ButtonType::Sepr,
             "",
             0,
             0,
             short(UI_UNIT_X),
             short(UI_UNIT_Y * 0.5f),
             nullptr,
             0.0f,
             0.0f,
             "");
  }

  block_layout_set_current(block, &layout);

  if (backing.disclosed) {
    grid_view_add_filter_search_row(layout, host, backing, panel_width);
  }
}

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

  const GridViewMetrics metrics = grid_view_resolve_metrics(
      state, tile_h, item_count, cols_est, host);
  const int visible_height = metrics.visible_height;
  const int effective_rows = metrics.effective_rows;
  const int total_rows = metrics.total_rows;
  const int scroll_offset_px = metrics.scroll_offset_px;

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
    grid_view_add_overlay_scrollbar(
        layout, grid_stack, state, visible_height, actual_cols, effective_rows, max_scroll_post);
  }

  /* --- Resize grip --- */

  if (host.show_grip) {
    grid_view_add_grip_row(layout, state, panel_width, host);
  }
}

static const ARegion *py_grid_layout_region(const bContext &C, const Block *block)
{
  /* Same region the grid geometry snapshot uses: popup when this layout is inside one,
   * otherwise the editor region. Matches #uiList living on the drawn #ARegion. */
  const ARegion *region = CTX_wm_region_popup(&C) ? CTX_wm_region_popup(&C) :
                                                    CTX_wm_region(&C);
  if (block && block->handle && block->handle->region) {
    region = block->handle->region;
  }
  return region;
}

static std::string py_grid_session_id(const ARegion *region,
                                      const StringRef kind,
                                      const StringRef type_or_empty,
                                      const StringRef grid_id)
{
  /* Region-scoped like #uiList DNA on #ARegion: same grid_id in two areas does not share
   * scroll. Kind prefixes (`pygrid` / `pygrid-asset`) let unregister drop a type's sessions. */
  if (type_or_empty.is_empty()) {
    return fmt::format("{}:{}:{}", kind, fmt::ptr(region), grid_id);
  }
  return fmt::format("{}:{}:{}:{}", kind, type_or_empty, fmt::ptr(region), grid_id);
}

/**
 * Identifier of the tile that stands for the host's active data-block, empty when \a lib_ref does
 * not hold it at all. Matching an #Image to an asset is a file-path comparison, the same one the
 * brush texture image grid uses (#image_grid_asset_represents_image); other ID types match by
 * identity.
 */
static std::string asset_grid_active_identifier(bContext &C,
                                                const AssetLibraryReference &lib_ref,
                                                const PointerRNA *active_id_ptr)
{
  if (active_id_ptr == nullptr || active_id_ptr->data == nullptr ||
      active_id_ptr->owner_id == nullptr)
  {
    return "";
  }
  ed::asset::list::storage_fetch(&lib_ref, &C);
  if (!ed::asset::list::is_loaded(&lib_ref)) {
    return "";
  }

  ID *active_id = active_id_ptr->owner_id;
  std::string identifier;
  ed::asset::list::iterate(lib_ref, [&](asset_system::AssetRepresentation &asset) {
    const bool matches = (asset.local_id() == active_id) ||
                         (GS(active_id->name) == ID_IM &&
                          ed::image_grid::image_grid_asset_represents_image(
                              asset, *reinterpret_cast<const Image *>(active_id)));
    if (matches) {
      identifier = asset.library_relative_identifier();
      return false;
    }
    return true;
  });
  return identifier;
}

/**
 * Scroll \a identifier into view once per change of library or active item, before the build
 * windows the items -- so switching to the library that holds the assigned data-block lands on it
 * instead of at the top. The focus key is only stored once the item was actually found, so a
 * library that is still reading retries on the redraw its notifier triggers.
 */
static void asset_grid_reveal_active(const bContext &C,
                                     GridSessionState &session,
                                     const AssetGridDataSource &source,
                                     const AssetLibraryReference &lib_ref,
                                     const StringRef identifier,
                                     const int cols,
                                     const int tile_h)
{
  const std::string focus_key = fmt::format(
      "{}|{}", ed::asset::library_reference_to_enum_value(&lib_ref), identifier);
  if (session.active_focus_key == focus_key) {
    return;
  }
  const int index = source.filtered_index_of(C, identifier);
  if (index < 0) {
    return;
  }

  const int row_top = (index / max_ii(1, cols)) * tile_h;
  const int viewport = max_ii(session.viewport_px, tile_h);
  int target = session.scroll_px;
  if (row_top < target) {
    target = row_top;
  }
  else if (row_top + tile_h > target + viewport) {
    target = row_top + tile_h - viewport;
  }
  session.scroll_px = max_ii(0, target);
  session.active_focus_key = focus_key;
}

namespace {

/** Tile size and column estimate, shared by both grid-view entry points. */
struct GridViewTileLayout {
  /** Where the grid itself goes: \a layout, or the box nested in it. */
  Layout *grid_layout = nullptr;
  int tile_w = 1;
  int tile_h = 1;
  int panel_width = 0;
  int cols_est = 1;
};

}  // namespace

/**
 * \param use_box: wrap the tiles in a themed box, matching #template_asset_image_grid. The box
 * adds #box_padding_px() on every side, so the width fed to the column math is shrunk by it up
 * front -- otherwise the drawn outline overflows the panel by that padding.
 */
static GridViewTileLayout grid_view_tile_layout(Layout &layout,
                                                const bool use_box,
                                                const int preview_size,
                                                const bool show_names)
{
  GridViewTileLayout tiles;
  tiles.grid_layout = use_box ? &layout.box() : &layout;
  const int box_padding = use_box ? tiles.grid_layout->box_padding_px() : 0;

  tiles.tile_w = preview_tile_size_x(preview_size);
  /* Tiles reserve room for the label only when names are shown. */
  tiles.tile_h = show_names ? preview_tile_size_y(preview_size) :
                              preview_tile_size_y_no_label(preview_size);
  tiles.panel_width = max_ii(layout.width() - 2 * box_padding, 0);
  tiles.cols_est = (tiles.panel_width > 0) ?
                       max_ii(1, tiles.panel_width / max_ii(tiles.tile_w, 1)) :
                       1;
  return tiles;
}

void template_grid_view_asset(Layout *layout,
                              bContext *C,
                              const char *grid_id,
                              PointerRNA *settings_ptr,
                              const char *activate_operator,
                              const char *drag_operator,
                              const GridViewAssetParams &params)
{
  if (!layout || !C || !grid_id || !grid_id[0] || !settings_ptr || !settings_ptr->data) {
    return;
  }

  Block *block = layout->block();

  /* Resolved by the same helper the query API uses, so #grid_query and the drawn grid can never
   * disagree about which items this grid holds or in what order (see
   * #asset_grid_source_from_settings). */
  AssetGridSourceParams source_params;
  source_params.activate_operator = activate_operator;
  source_params.drag_operator = drag_operator;
  source_params.use_drag = params.use_drag;
  source_params.activate_context_id = params.activate_context_id;
  source_params.membership_shelf_idname = params.membership_shelf_idname;
  source_params.catalog_memory_domain = params.catalog_memory_domain;
  source_params.catalog_filter_domain = params.catalog_filter_domain;

  const int preview_size = grid_settings::preview_size_get(*settings_ptr);
  const bool show_names = grid_settings::show_names_get(*settings_ptr);

  const AssetLibraryReference lib_ref = asset_grid_library_from_settings(*settings_ptr,
                                                                         source_params);
  const std::string active_identifier = asset_grid_active_identifier(
      *C, lib_ref, params.active_id_ptr);

  const GridViewTileLayout tiles = grid_view_tile_layout(
      *layout, params.use_box, preview_size, show_names);
  const int cols_est = tiles.cols_est;
  const int tile_h = tiles.tile_h;

  source_params.active_identifier = active_identifier.c_str();
  std::unique_ptr<AssetGridDataSource> source = asset_grid_source_from_settings(*settings_ptr,
                                                                               source_params);
  /* Stays valid past the move below: the view owns the source for the whole build. */
  AssetGridDataSource *source_ptr = source.get();

  const std::string session_id = py_grid_session_id(
      py_grid_layout_region(*C, block), "pygrid-asset", "", grid_id);

  auto view_unique = std::make_unique<GenericGridView>(
      *C, std::move(source), cols_est, session_id);
  GenericGridView *view_ptr = view_unique.get();
  view_unique->set_tile_size(tiles.tile_w, tiles.tile_h);
  /* Lets the widget code scale the label font down for small previews, the way the Asset Shelf
   * tiles do, instead of sizing it from the whole tile. */
  view_unique->set_preview_size_px(preview_size);
  view_unique->set_cols_per_row_hint(cols_est);

  AbstractGridView *grid_view = block_add_view(*block, session_id, std::move(view_unique));

  /* Recorded every build, including when it is empty, so #UI_OT_grid_view_step always steps from
   * what the grid currently shows as active. */
  grid_session_state_ensure(session_id).active_identifier = active_identifier;

  if (!active_identifier.empty()) {
    asset_grid_reveal_active(*C,
                             grid_session_state_ensure(session_id),
                             *source_ptr,
                             lib_ref,
                             active_identifier,
                             cols_est,
                             tile_h);
  }

  block_add_dynamic_listener(block, ed::asset::list::asset_reading_region_listen_fn);
  block_add_dynamic_listener(block, grid_view_block_listener);

  ViewGridStateAccess state_access(*view_ptr, session_id);
  GridViewHostParams host_params;
  host_params.filter_settings = settings_ptr;

  build_grid_view(*C,
                  *tiles.grid_layout,
                  *grid_view,
                  state_access,
                  view_ptr->get_cached_item_count_for_build(),
                  cols_est,
                  tiles.panel_width,
                  host_params);
}

void template_grid_view_custom(Layout *layout,
                               bContext *C,
                               const char *grid_id,
                               const char *gridtype_name,
                               PointerRNA *dataptr,
                               const char *propname,
                               PointerRNA *settings_ptr,
                               const GridViewCustomParams &params)
{
  if (!layout || !C || !grid_id || !grid_id[0] || !gridtype_name || !gridtype_name[0]) {
    return;
  }

  uiGridType *grid_type = WM_uigridtype_find(gridtype_name, false);
  if (!grid_type) {
    RNA_warning("Grid type %s not found", gridtype_name);
    return;
  }

  PointerRNA empty_data = PointerRNA_NULL;
  if (!dataptr) {
    dataptr = &empty_data;
  }
  const char *propname_use = propname ? propname : "";
  /* Collection is optional: #UIGrid.get_item_count / #get_item are the source. When a
   * property name is given it must exist, but it need not be a collection. */
  if (propname_use[0] && dataptr->data && dataptr->type) {
    if (!RNA_struct_find_property(dataptr, propname_use)) {
      RNA_warning("Property not found: %s.%s", RNA_struct_identifier(dataptr->type), propname_use);
      return;
    }
  }

  Block *block = layout->block();

  const int preview_size = (settings_ptr && settings_ptr->data) ?
                               grid_settings::preview_size_get(*settings_ptr) :
                               96;
  const bool show_names = (settings_ptr && settings_ptr->data) ?
                              grid_settings::show_names_get(*settings_ptr) :
                              false;

  const GridViewTileLayout tiles = grid_view_tile_layout(
      *layout, params.use_box, preview_size, show_names);

  const std::string session_id = py_grid_session_id(
      py_grid_layout_region(*C, block), "pygrid", gridtype_name, grid_id);

  const bool has_settings = settings_ptr && settings_ptr->data;
  auto source = std::make_unique<PyCallbackGridDataSource>(
      grid_type,
      *dataptr,
      propname_use,
      show_names,
      params.active_identifier ? params.active_identifier : "",
      has_settings ? grid_settings::filter_search_get(*settings_ptr) : "");

  auto view_unique = std::make_unique<GenericGridView>(
      *C, std::move(source), tiles.cols_est, session_id, grid_type);
  GenericGridView *view_ptr = view_unique.get();
  view_unique->set_tile_size(tiles.tile_w, tiles.tile_h);
  /* Same label-font scaling the Asset Shelf tiles get; see #template_grid_view_asset. */
  view_unique->set_preview_size_px(preview_size);
  view_unique->set_cols_per_row_hint(tiles.cols_est);

  AbstractGridView *grid_view = block_add_view(*block, session_id, std::move(view_unique));

  ViewGridStateAccess state_access(*view_ptr, session_id);
  /* A grid type that draws its own filter keeps doing so instead of getting the built-in row --
   * two filter rows under one grid would be a duplicate, not a choice. */
  GridViewHostParams host_params;
  if (has_settings && !grid_type->draw_filter) {
    host_params.filter_settings = settings_ptr;
  }

  build_grid_view(*C,
                  *tiles.grid_layout,
                  *grid_view,
                  state_access,
                  view_ptr->get_cached_item_count_for_build(),
                  tiles.cols_est,
                  tiles.panel_width,
                  host_params);

  if (grid_type->draw_filter) {
    uiGrid grid_inst;
    grid_inst.type = grid_type;
    grid_type->draw_filter(&grid_inst, C, *layout);
  }
}

} /* namespace blender::ui */
