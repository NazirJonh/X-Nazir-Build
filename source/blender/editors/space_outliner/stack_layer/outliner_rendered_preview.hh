/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Previews the outliner renders itself, from the state of a material as it is now rather than from
 * the thumbnail a data-block carries. A caller describes what to show (#RenderedPreview) and asks
 * to draw it where a preview belongs; this module owns the pixels, asks the stock ID preview
 * machinery to render them only when the description's hash changed, and keeps the previous render
 * on show until the new one lands.
 *
 * Adding a new use needs nothing here: build a #RenderedPreview whose hash folds in everything the
 * picture depends on, draw it with #rendered_preview_draw and add its tooltip with
 * #rendered_preview_tooltip_add.
 */

#pragma once

#include <cstdint>
#include <string>

#include "DNA_vec_types.h"

struct bContext;

namespace blender {

namespace ui {
struct Block;
}

namespace ed::outliner {

enum class RenderedPreviewKind : int8_t {
  /** The preview of the data-block #RenderedPreview::id_uid names, drawn like an ID picker's
   * thumbnail. Nothing is rendered here: the data-block's own change flag decides. */
  Id = 0,
  /** A lit sphere shaded with the description's constants or, where #RenderedPreview::map_uid
   * names maps, with those. */
  Sphere,
  /** The material #RenderedPreview::id_uid names, rendered as it is now: its whole graph. The
   * thumbnail it carries (for an asset, the one stored with it) stands in until the first render
   * lands, and the previous render until the next one does. */
  Material,
};

/** Everything a rendered preview shows. Cheap to copy: a tooltip holds a copy of it. */
struct RenderedPreview {
  RenderedPreviewKind kind = RenderedPreviewKind::Id;
  /**
   * Folds in every input the render reads: the pixels are rendered again only when it differs from
   * the one they were rendered for. Meaningless for #RenderedPreviewKind::Id.
   */
  uint64_t hash = 0;
  /** The data-block by session UID: the material of #RenderedPreviewKind::Material, or what
   * #RenderedPreviewKind::Id previews. */
  uint32_t id_uid = 0;
  short id_type = 0;
  /** Scene linear. The Sphere's base colour, and the flat colour that answers wherever nothing
   * else can (before a render, with no pixels at all). */
  float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  /** What #RenderedPreviewKind::Sphere shades with. */
  float metallic = 0.0f;
  float roughness = 0.5f;
  /** Maps by session UID (Base Color, Metallic, Roughness), or zero for the matching constant. */
  uint32_t map_uid[3] = {0, 0, 0};
  /** The tooltip's title. */
  std::string label;
};

/**
 * Draw \a preview in \a preview_rect. Returns false when there is nothing to preview at all, so
 * the caller's other looks answer. While pixels are on their way, \a preview's colour stands in.
 */
bool rendered_preview_draw(const bContext *C,
                           const rctf &preview_rect,
                           const RenderedPreview &preview,
                           float alpha_fac);

/**
 * A tooltip over \a preview_rect with the preview's label and a larger picture of it. Added in the
 * block beside the draw, since the block is rebuilt on every redraw. #RenderedPreviewKind::Id
 * gets none.
 */
void rendered_preview_tooltip_add(ui::Block *block,
                                  const rctf &preview_rect,
                                  const RenderedPreview &preview);

}  // namespace ed::outliner
}  // namespace blender
