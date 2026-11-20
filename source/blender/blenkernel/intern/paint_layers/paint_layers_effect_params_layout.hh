/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The packed positions of a filter's parameters in the #composite_filter_params fold -- the one
 * place that names them. The fold walks the kind's schema order and lays a colour's three channels
 * across three consecutive slots (see #paint_layers_composite.cc), so a scalar before the first colour
 * keeps the kind's own enum index while one after it sits at a packed position the enum does not
 * name; the blocks below name every position, the colour's slots included. A parameter added to a
 * kind moves the packed positions of the ones after it, which the length asserts below break on:
 * the block is the single place to update, not every reader.
 */

#pragma once

#include "BKE_paint_material_composite.hh"

#include "paint_layers_kind.hh"

namespace blender::bke::paint_filter_params {

/** The slots a colour occupies in the fold: its three channels, the schema order kept. */
constexpr int color_slots = 3;

/** The extra slots a colour adds over a scalar, so the fold's length is the schema's own plus two
 * per colour. */
constexpr int color_extra = color_slots - 1;

/** The packed positions of the Color Match filter's parameters: two colours sit ahead of the
 * variation's four scalars, so the variation rides four past the schema's own number. */
struct ColorMatch {
  static constexpr int source_picked = PAINT_LAYER_COLOR_MATCH_SOURCE_PICKED;
  static constexpr int source_color = PAINT_LAYER_COLOR_MATCH_SOURCE_COLOR;
  static constexpr int target_color = source_color + color_slots;
  static constexpr int variation = target_color + color_slots;
  static constexpr int variation_hue = variation + 1;
  static constexpr int variation_chroma = variation + 2;
  static constexpr int variation_luma = variation + 3;
  /** The slots the fold fills past the schema's own number of parameters. */
  static constexpr int packed_num = variation_luma + 1;
};
static_assert(ColorMatch::packed_num == PAINT_LAYER_COLOR_MATCH_PARAM_NUM + 2 * color_extra,
              "the Color Match layout block is out of date with its schema");

/** The packed positions of the Fill Area filter's parameters: the fill colour sits ahead of the
 * blur and the iterations, so both ride past the schema's own numbers. */
struct FillArea {
  static constexpr int source = PAINT_LAYER_FILL_AREA_SOURCE;
  static constexpr int threshold = PAINT_LAYER_FILL_AREA_THRESHOLD;
  static constexpr int debug = PAINT_LAYER_FILL_AREA_DEBUG;
  static constexpr int uv_border = PAINT_LAYER_FILL_AREA_UV_BORDER;
  static constexpr int color_mode = PAINT_LAYER_FILL_AREA_COLOR_MODE;
  static constexpr int color = PAINT_LAYER_FILL_AREA_COLOR;
  static constexpr int blur = color + color_slots;
  static constexpr int iterations = blur + 1;
  /** The slots the fold fills past the schema's own number of parameters. */
  static constexpr int packed_num = iterations + 1;
};
static_assert(FillArea::packed_num == PAINT_LAYER_FILL_AREA_PARAM_NUM + color_extra,
              "the Fill Area layout block is out of date with its schema");

/* The three remaining colour carriers end their schema with the colour, so nothing sits past it
 * and the kind's own enum already names the packed position; these ties hold the invariant (a
 * parameter appended after the colour would break them and the readers by enum with it). */
static_assert(PAINT_LAYER_GLOW_COLOR + 1 == PAINT_LAYER_GLOW_PARAM_NUM,
              "the Glow colour is no longer last in its schema");
static_assert(PAINT_LAYER_DROP_SHADOW_COLOR + 1 == PAINT_LAYER_DROP_SHADOW_PARAM_NUM,
              "the Drop Shadow colour is no longer last in its schema");
static_assert(PAINT_LAYER_MASK_OUTLINE_COLOR + 1 == PAINT_LAYER_MASK_OUTLINE_PARAM_NUM,
              "the Mask Outline colour is no longer last in its schema");

/* The fold writes into the fixed #filter_params arrays, so a layout longer than the array would
 * truncate silently. */
static_assert(ColorMatch::packed_num <= PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM);
static_assert(FillArea::packed_num <= PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM);

}  // namespace blender::bke::paint_filter_params
