/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editorui
 *
 * API for simple creation of grid UIs, supporting typically needed features.
 * https://developer.blender.org/docs/features/interface/views/grid_views/
 */

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "BLI_function_ref.hh"
#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "UI_abstract_view.hh"
#include "UI_resources.hh"

namespace blender {

struct bContext;
struct View2D;

namespace ui {

class AbstractGridView;
class GridViewItemDropTarget;
struct GridSessionState;

struct Layout;

/* ---------------------------------------------------------------------- */
/** \name Grid-View Item Type
 * \{ */

class AbstractGridViewItem : public AbstractViewItem {
  friend class AbstractGridView;
  friend class GridViewLayoutBuilder;

 protected:
  /**
   * A string that uniquely identifies this item in the view.
   *
   * Ideally this would just be a StringRef to save memory. This was made a
   * std::string to fix #141882 in a relatively safe way. */
  std::string identifier_{};

 public:
  /* virtual */ ~AbstractGridViewItem() override = default;

  virtual void build_grid_tile(const bContext &C, Layout &layout) const = 0;

  /* virtual */ std::optional<std::string> debug_name() const override;

  AbstractGridView &get_view() const;

 protected:
  AbstractGridViewItem(StringRef identifier);

  /** See AbstractViewItem::matches(). */
  /* virtual */ bool matches(const AbstractViewItem &other) const override;

  /* virtual */ std::unique_ptr<DropTargetInterface> create_item_drop_target() final;
  virtual std::unique_ptr<GridViewItemDropTarget> create_drop_target();

 private:
  void add_grid_tile_button(Block &block);
};

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Grid-View Base Class
 * \{ */

struct GridViewStyle {
  GridViewStyle(int width, int height);
  int tile_width = 0;
  int tile_height = 0;
};

class AbstractGridView : public AbstractView {
  friend class AbstractGridViewItem;
  friend class GridViewBuilder;
  friend class GridViewLayoutBuilder;

 protected:
  Vector<std::unique_ptr<AbstractGridViewItem>> items_;
  /** Store this to avoid recomputing. */
  mutable std::optional<int> item_count_filtered_;
  /** <identifier, item> map to lookup items by identifier, used for efficient lookups in
   * #update_from_old(). */
  Map<StringRef, AbstractGridViewItem *> item_map_;
  GridViewStyle style_;
  int cols_per_row_ = 0;
  /** When > 0, overrides width-based column guess in #GridViewLayoutBuilder. */
  int cols_per_row_hint_ = 0;
  /** When set, empty row spacers are added so the grid block is at least this tall (pixels). */
  std::optional<int> min_viewport_height_;
  /**
   * Popover/fixed-height layouts: only the visible tile rows are placed (no scroll spacer labels).
   * The scroll position is stored in the grid_id-keyed session registry (#use_session_scroll)
   * instead of the region #View2D, mirroring #AbstractTreeView's row-index approach. This is
   * robust against the popup draw pipeline re-initializing the region #View2D every refresh, and
   * against the per-refresh view rebuild (the rebuilt view re-attaches to the same session).
   */
  bool fixed_viewport_layout_ = false;
  /**
   * Session-registry entry this view attached to via #use_session_scroll; null otherwise. Owned
   * by the registry (stable address, refcounted by attached views); holds the pixel scroll
   * position #scroll_px() operates on.
   */
  GridSessionState *session_ = nullptr;
  /**
   * The grid_id this view attached to via #use_session_scroll. The unified input handler resolves
   * a hit view back to its session by this key (it may differ from the #ViewLink idname, e.g. the
   * per-shelf key the asset shelf popover uses).
   */
  std::string session_grid_id_;
  /** Request scrolling the active item into view during the next fixed-viewport build. */
  bool scroll_active_into_view_on_build_ = false;
  /** Remember the center intent for the deferred #scroll_active_into_view_on_build_ pass. */
  bool scroll_active_center_on_build_ = false;

 public:
  AbstractGridView();
  /* virtual */ ~AbstractGridView() override;

  using ItemIterFn = FunctionRef<void(AbstractGridViewItem &)>;
  void foreach_item(ItemIterFn iter_fn) const;
  void foreach_filtered_item(ItemIterFn iter_fn) const;

  /**
   * Convenience wrapper constructing the item by forwarding given arguments to the constructor of
   * the type (\a ItemT).
   *
   * E.g. if your grid-item type has the following constructor:
   * \code{.cpp}
   * MyGridItem(std::string str, int i);
   * \endcode
   * You can add an item like this:
   * \code
   * add_item<MyGridItem>("blabla", 42);
   * \endcode
   */
  template<class ItemT, typename... Args> inline ItemT &add_item(Args &&...args);
  const GridViewStyle &get_style() const;
  int get_item_count() const;
  int get_item_count_filtered() const;
  int cols_per_row() const
  {
    return cols_per_row_;
  }

  void set_tile_size(int tile_width, int tile_height);
  /** Fixed column count (e.g. from #template_asset_image_grid `cols`). 0 = guess from layout
   * width. */
  void set_cols_per_row_hint(int cols);
  /** Ensure the laid-out grid uses at least this height, using the same row spacers as scrolling.
   */
  void set_min_viewport_height(int height_px);
  [[nodiscard]] std::optional<int> min_viewport_height() const;
  void set_fixed_viewport_layout(bool fixed_viewport_layout);
  [[nodiscard]] bool use_fixed_viewport_layout() const;
  /**
   * Menu-style scroll zones (#UI_MENU_SCROLL_MOUSE) over the grid bounds in block space, extended
   * into the separator gaps so the persistent scroll arrows are themselves hoverable. Used for edge
   * auto-scroll. The current scroll row derives from #scroll_px().
   */
  [[nodiscard]] std::optional<ViewScrollDirection> fixed_viewport_scroll_at_y(
      const Block &block, float block_space_y) const;
  void draw_overlays(const ARegion &region, const Block &block) const override;
  AbstractViewItem *find_active_or_visible_item() const override;
  bool supports_scrolling() const override;
  bool is_fully_visible() const override;
  void scroll(ViewScrollDirection direction) override;
  /**
   * Attach this view to the grid_id-keyed session registry entry (creating it on first use) so
   * the scroll position survives the per-refresh view rebuild and popover reopen. Call once,
   * right after construction/registration. Two views passing the same \a grid_id deliberately
   * share one state.
   */
  void use_session_scroll(StringRef grid_id);
  /** The session key this view attached to (empty when #use_session_scroll was not called). */
  [[nodiscard]] StringRef session_grid_id() const
  {
    return session_grid_id_;
  }
  /**
   * Snapshot this fixed-viewport grid's geometry into its session so the unified input handler can
   * hit-test and clamp scrolling from stable state (independent of the tiles built this frame),
   * matching the embedded host's #GridStateAccess::geometry_store. No-op without a session.
   */
  void store_fixed_viewport_session_geometry(const ARegion *region);
  /**
   * Pixel-exact scroll position, the single source of truth for scrolling. Whole rows and the
   * sub-row clip offset are derived on demand (`scroll_px() / tile_height`,
   * `scroll_px() % tile_height`); the sub-row remainder on the last page emerges naturally from
   * the pixel-exact `[0, max_scroll_px()]` clamp. Returns 0 / ignores writes when the view has
   * no session (#use_session_scroll not called).
   */
  [[nodiscard]] virtual int scroll_px() const;
  virtual void scroll_px_set(int px);
  /** Stable pointer to the session's pixel scroll position for the overlay #ButtonType::Scroll
   * widget binding (pixel-scale); null when the view has no session. */
  [[nodiscard]] int *session_scroll_px_ptr();
  /** Total pixel scroll range; fixed-viewport layouts derive it from the viewport geometry. */
  [[nodiscard]] virtual int max_scroll_px() const;
  /** Total pixel scroll range of the fixed viewport (see #FixedViewportGeometry). */
  [[nodiscard]] int fixed_viewport_max_scroll_px() const;
  AbstractViewItem *navigate_left(AbstractViewItem *from) override;
  AbstractViewItem *navigate_right(AbstractViewItem *from) override;
  AbstractViewItem *navigate_up(AbstractViewItem *from) override;
  AbstractViewItem *navigate_down(AbstractViewItem *from) override;

  void scroll_active_into_view(bContext *C, bool scroll_active_to_center = false) override;
  void scroll_active_into_center(bContext *C);

  bool scroll_active_into_center_on_draw_ = false;

  IndexRange get_visible_range(const View2D &v2d,
                               const AbstractGridViewItem *force_visible_item) const;
  /** Item index range to build for the current fixed-viewport scroll position (#scroll_px). */
  IndexRange fixed_viewport_visible_range() const;

 protected:
  virtual void build_items() = 0;

 private:
  /** Row/column geometry of the fixed viewport for the current filtered item count. */
  struct FixedViewportGeometry {
    int cols;
    int content_rows;
    /** Whole tile rows that fit fully in the viewport (floor of #viewport_height / tile). */
    int visible_rows;
    /** Highest possible first visible row (0 when everything fits). */
    int max_first_row;
    /**
     * Pixel height of the viewport. May exceed #visible_rows * tile_height, in which case a
     * partial bottom row is drawn clipped (see #GridViewLayoutBuilder::build_from_view).
     */
    int viewport_height;
    /**
     * Total pixel scroll range: whole content height minus the pixel-exact #viewport_height, so
     * scrolling stops exactly at the content end (revealing the partial bottom row fully) with no
     * over- or under-scroll. Decomposed into #max_first_row whole rows plus a sub-row remainder.
     */
    int max_scroll_px;
  };
  FixedViewportGeometry fixed_viewport_geometry() const;
  /** Whole rows scrolled out of view at the top, derived from #scroll_px(). */
  [[nodiscard]] int scroll_value() const;
  /** Sub-row clip offset in pixels, derived from #scroll_px(). */
  [[nodiscard]] int scroll_offset_px() const;
  /** First visible row, read (and clamped) from #scroll_px(). */
  int fixed_viewport_first_row() const;
  /** Re-clamp #scroll_px() against the current viewport geometry (item count/columns changed). */
  void fixed_viewport_clamp_scroll_value();
  /** Set #scroll_px() so the active item's row is within the fixed viewport. */
  void fixed_viewport_scroll_active_into_view(bool scroll_active_to_center);

  void foreach_view_item(FunctionRef<void(AbstractViewItem &)> iter_fn) const final;
  void update_children_from_old(const AbstractView &old_view) override;
  AbstractGridViewItem *find_matching_item(const AbstractGridViewItem &item_to_match,
                                           const AbstractGridView &view_to_search_in) const;

  /**
   * Add an already constructed item, moving ownership to the grid-view.
   * All items must be added through this, it handles important invariants!
   */
  AbstractGridViewItem &add_item(std::unique_ptr<AbstractGridViewItem> item);
};

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Drag & Drop
 * \{ */

/**
 * Class to define the behavior when dropping something onto/into a view item, plus the behavior
 * when dragging over this item. An item can return a drop target for itself via a custom
 * implementation of #AbstractGridViewItem::create_drop_target().
 */
class GridViewItemDropTarget : public DropTargetInterface {
 protected:
  AbstractGridView &view_;

 public:
  GridViewItemDropTarget(AbstractGridView &view);

  /** Request the view the item is registered for as type #ViewType. Throws a `std::bad_cast`
   * exception if the view is not of the requested type. */
  template<class ViewType> inline ViewType &get_view() const;
};

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Grid-View Builder
 *
 *  TODO unify this with `TreeViewBuilder` and call view-specific functions via type erased view?
 * \{ */

class GridViewBuilder {
 public:
  GridViewBuilder(Block &block);

  void build_grid_view(const bContext &C,
                       AbstractGridView &grid_view,
                       Layout &layout,
                       std::optional<StringRef> search_string = {},
                       const View2D *v2d_override = nullptr);
};

/** \} */

/**
 * Register the pre-button handler that powers touch/pen drag-scroll and mouse-wheel scroll for
 * every #AbstractGridView built via #build_grid_view, regardless of grid_id or owning editor.
 * Called once from #ED_spacetypes_init(); not tied to any space type.
 */
void grid_view_register_pre_button_handler();

/**
 * Drop the session-state registry entry for \a grid_id if nothing references it. For
 * per-space dynamic grid_ids whose owner is being freed (e.g. a #View3D closing).
 */
void grid_view_session_remove(StringRef grid_id);

/**
 * Reset an existing session's scroll to the top and drop its per-column pins. For owners (e.g. a
 * #View3D grid) that must jump to the top when their content set changes (filter / library /
 * catalog switch). No-op when the session does not exist yet.
 */
void grid_view_session_reset_scroll(StringRef grid_id);

/**
 * Column count of the session with \a grid_id, or 0 when it was never drawn. Lets a space that
 * keeps its own per-grid bookkeeping (e.g. the View3D focus-applied flags) read a grid's layout
 * without reaching into the interface-internal session registry.
 */
int grid_view_session_cols(StringRef grid_id);

/**
 * True when \a xy is over the overlay scrollbar of the session grid \a grid_id in \a region.
 * Hit-tests the scrollbar for space-specific hotkeys (e.g. View3D numpad-focus) without exposing
 * the session's transitional scroll-widget field.
 */
bool grid_view_session_scroll_button_under_mouse(const ARegion *region,
                                                 const int xy[2],
                                                 StringRef grid_id);

/**
 * Height (in #UI_UNIT_Y) for a fixed-viewport grid inside a popover so the whole popover stays
 * within the window even when the spawning button is zoomed (#Block::aspect < 1). Returns
 * \a default_units when the popover is not zoomed (the full-height popover fits). The result is
 * snapped to whole tile rows so the viewport shows complete rows with no trailing gap.
 *
 * \param non_grid_units: header rows and gaps consumed before/after the grid (#UI_UNIT_Y).
 * \param tile_units: one tile row height in #UI_UNIT_Y.
 * \param default_units: viewport height used when the popover is not zoomed.
 */
float popup_grid_fixed_viewport_units(const bContext *C,
                                      const Block *block,
                                      float non_grid_units,
                                      float tile_units,
                                      float default_units);

/* ---------------------------------------------------------------------- */
/** \name Predefined Grid-View Item Types
 *
 *  Common, Basic Grid-View Item Types.
 * \{ */

/**
 * A grid item that shows preview image icons at a nicely readable size (multiple of the normal UI
 * unit size).
 */
class PreviewGridItem : public AbstractGridViewItem {
 public:
  using IsActiveFn = std::function<bool()>;
  using ActivateFn = std::function<void(bContext &C, PreviewGridItem &new_active)>;

 protected:
  /** See #set_on_activate_fn() */
  ActivateFn activate_fn_;
  /** See #set_is_active_fn() */
  IsActiveFn is_active_fn_;
  bool hide_label_ = false;

 public:
  std::string label;
  int preview_icon_id = ICON_NONE;

  PreviewGridItem(StringRef identifier, StringRef label, int preview_icon_id);

  void build_grid_tile(const bContext &C, Layout &layout) const override;

  void build_grid_tile_button(Layout &layout,
                              BIFIconID override_preview_icon_id = ICON_NONE) const;

  /**
   * Set a custom callback to execute when activating this view item. This way users don't have to
   * sub-class #PreviewGridItem, just to implement custom activation behavior (a common thing to
   * do).
   */
  void set_on_activate_fn(ActivateFn fn);
  /**
   * Set a custom callback to check if this item should be active.
   */
  void set_is_active_fn(IsActiveFn fn);

  void hide_label();

 private:
  std::optional<bool> should_be_active() const override;
  void on_activate(bContext &C) override;
};

/** \} */

/* ---------------------------------------------------------------------- */

template<class ItemT, typename... Args> inline ItemT &AbstractGridView::add_item(Args &&...args)
{
  static_assert(std::is_base_of_v<AbstractGridViewItem, ItemT>,
                "Type must derive from and implement the AbstractGridViewItem interface");

  return dynamic_cast<ItemT &>(add_item(std::make_unique<ItemT>(std::forward<Args>(args)...)));
}

template<class ViewType> ViewType &GridViewItemDropTarget::get_view() const
{
  static_assert(std::is_base_of_v<AbstractGridView, ViewType>,
                "Type must derive from and implement the ui::AbstractGridView interface");
  return dynamic_cast<ViewType &>(view_);
}

}  // namespace ui
}  // namespace blender
