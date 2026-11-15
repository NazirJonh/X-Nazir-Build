/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The kind tables of the paint layers source: the look a row wears and the filters the Add Effect
 * menu offers. The data lives apart from the row builder so the source stays within the module's
 * size budget; the rows, the effects and the Add menu all read the same tables, so a kind cannot
 * read one way in the menu and another in the stack.
 */

#pragma once

#include <cstdint>

#include "BLI_span.hh"

#include "DNA_material_types.h"

namespace blender::ed::outliner {

/**
 * Material Symbols codepoints the paint stack draws its icons from.
 *
 * The codepoints come from `scripts/startup/bl_ui/glyph_library/data/material_symbols.json`; the
 * font that renders them ships with Blender, so a codepoint is stable for the bundled library
 * version. Each icon keeps its built-in #ICON_* as the fallback for headless builds and as the
 * name a user finds in the rest of Blender.
 *
 * #BRUSH, #FILL and #GRADIENT are the three `space_outliner.py` repeats for its header buttons.
 * `outliner_stack_source_paint_layers_test.cc` pins their values, so a change here fails the test
 * and sends the reader to the Python comment that names the test. The values are defined in
 * `outliner_stack_paint_filter_table.cc`, the one place the look tables read them.
 */
namespace paint_glyph {
extern const uint32_t BRUSH;
extern const uint32_t FILL;
extern const uint32_t GRADIENT;
extern const uint32_t TEXTURE;
extern const uint32_t INVERT;
extern const uint32_t LEVELS;
extern const uint32_t BRIGHTNESS;
extern const uint32_t HSL;
extern const uint32_t ANCHOR;
extern const uint32_t CELL_TOWER;
extern const uint32_t COLORIZE;
extern const uint32_t SELECT_ALL;
extern const uint32_t PATTERN;
}  // namespace paint_glyph

/** The pair a paint row shows for its kind: the built-in icon and its Material Symbols glyph. */
struct PaintKindLook {
  int icon;
  /** Material Symbols codepoint, or 0 when the kind has no glyph and shows #icon alone. */
  uint32_t glyph;
};

/**
 * The role a row of one source plays. Only a Paint row looks different in the two -- the mask's
 * Add entry names it by the Grease Pencil icon -- but every row asks through this one function so
 * a role cannot drift from the Add menu that names it.
 */
enum class PaintLookRole {
  /** A layer's content, or a correction laid over it. */
  Content,
  /** A correction laid over the layer's mask. */
  MaskItem,
};

/**
 * A filter the Add Effect menu offers. The vocabulary is the editor's (a name and a tooltip);
 * which filters exist is #eMaterialPaintLayerFilter. The icon and glyph come from
 * #paint_look_for_source, the same place a row asks, so the menu and the row cannot disagree.
 * #row_add reads the position back, so a new filter is one entry here and one in the kernel's kind
 * table.
 */
struct PaintFilterAddKind {
  const char *identifier;
  const char *name;
  const char *description;
  int16_t filter;
};

/**
 * The icon and glyph every row and Add entry of \a source shows, in \a role. This is the one place
 * the pair is written: the rows, the effects and the Add menu all ask here, so a kind cannot read
 * one way in the menu and another in the stack.
 */
PaintKindLook paint_look_for_source(eMaterialPaintLayerSource source,
                                    PaintLookRole role,
                                    int16_t subtype = 0);

/** The look of an Anchor Point marker, whichever list it sits in. */
PaintKindLook paint_look_for_anchor_point();

/** The look of an Anchor Point reader: what it lays, not the marker it reads. */
PaintKindLook paint_look_for_anchor_reader();

/** The pointwise and map filters the Add Effect menu offers in its main and Filter lists. */
Span<const PaintFilterAddKind> paint_filter_add_kinds();

/** The CPU-computed filters the Add Effect menu offers as corrections of a row's colour. */
Span<const PaintFilterAddKind> paint_effect_add_kinds();

/** The map-driven filters the Add Effect menu offers over a mask. */
Span<const PaintFilterAddKind> paint_coverage_add_kinds();

/**
 * Whether a filter is listed in the "Filter" submenu of the Add Effect menu instead of the main
 * list. The submenu holds the filters added after the original set, so the main list keeps the
 * place the older ones have always had; a filter not named here stays in the main list rather than
 * disappearing from the menu.
 */
bool paint_filter_is_new(int16_t filter);

/**
 * Whether a filter is listed in the "Filter 2" submenu of the Add Effect menu: the Painter-parity
 * filters, #MA_PAINT_LAYER_FILTER_HISTOGRAM_SCAN and every value after it. Never together with
 * #paint_filter_is_new.
 */
bool paint_filter_is_filter2(int16_t filter);

/** The transform effects the Add Effect menu lists under "Transform" instead of the other lists. */
bool paint_filter_is_transform(int16_t filter);

}  // namespace blender::ed::outliner
