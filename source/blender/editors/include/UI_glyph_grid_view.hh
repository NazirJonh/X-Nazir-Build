/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editorui
 *
 * Grid view for displaying and selecting glyphs.
 */

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "UI_grid_view.hh"

namespace blender {

struct bContext;

namespace ui {

/* ---------------------------------------------------------------------- */
/** \name Glyph Grid Item
 * \{ */

/**
 * A grid item that displays a single glyph.
 * Extends AbstractGridViewItem to show unicode glyphs in a grid format.
 */
class GlyphGridItem : public AbstractGridViewItem {
 public:
  using OnSelectFn = std::function<void(bContext &C, const std::string &unicode)>;

 protected:
  std::string unicode_;
  std::string name_;
  OnSelectFn on_select_fn_;

 public:
  GlyphGridItem(StringRef identifier, StringRef unicode, StringRef name);

  void build_grid_tile(const bContext &C, Layout &layout) const override;

  /**
   * Set a callback to execute when this glyph is selected.
   */
  void set_on_select_fn(OnSelectFn fn);

  const std::string &get_unicode() const { return unicode_; }
  const std::string &get_name() const { return name_; }

 private:
  std::optional<bool> should_be_active() const override;
  void on_activate(bContext &C) override;
};

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Glyph Grid View
 * \{ */

/**
 * A grid view for displaying glyphs.
 * Manages a collection of GlyphGridItem instances and handles their layout.
 */
class GlyphGridView : public AbstractGridView {
 public:
  using OnGlyphSelectFn = std::function<void(bContext &C, const std::string &unicode)>;

 protected:
  Vector<std::pair<std::string, std::string>> glyphs_;  // (unicode, name) pairs
  OnGlyphSelectFn on_glyph_select_fn_;
  std::string search_filter_;

 public:
  GlyphGridView();

  /**
   * Add glyphs to the grid view.
   * \param glyphs: Vector of (unicode, name) pairs
   */
  void set_glyphs(const Vector<std::pair<std::string, std::string>> &glyphs);

  /**
   * Set the search filter string.
   * Filters glyphs by name (unicode or display name).
   */
  void set_search_filter(StringRef search_filter);

  /**
   * Set a callback to execute when a glyph is selected.
   */
  void set_on_glyph_select_fn(OnGlyphSelectFn fn);

  const std::string &get_search_filter() const { return search_filter_; }

 protected:
  void build_items() override;
};

/** \} */

}  // namespace ui
}  // namespace blender
