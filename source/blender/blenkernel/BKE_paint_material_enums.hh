/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The small enums shared by the paint-material code that is independent of which model -- the old
 * graph-truth stack or the new DNA description -- is in play. They are named here rather than in
 * either model's header so `BKE_paint.hh` and the composite can name them without the old model.
 */

#include <cstdint>

namespace blender {

/**
 * How a layer combines with what is below it: every mode a Mix node offers.
 *
 * The values match the `MA_RAMP_*` codes the node itself stores, so the description's one table
 * (#BKE_paint_layers_blend_to_ramp) maps straight onto this and no second table can drift from it.
 * #NormalCombine is the one non-Mix operation and sits past the end of the ramp range.
 */
enum class CompositeBlend : int8_t {
  Mix = 0,          /* MA_RAMP_BLEND */
  Add = 1,          /* MA_RAMP_ADD */
  Multiply = 2,     /* MA_RAMP_MULT */
  Subtract = 3,     /* MA_RAMP_SUB */
  Screen = 4,       /* MA_RAMP_SCREEN */
  Divide = 5,       /* MA_RAMP_DIV */
  Difference = 6,   /* MA_RAMP_DIFF */
  Darken = 7,       /* MA_RAMP_DARK */
  Lighten = 8,      /* MA_RAMP_LIGHT */
  Overlay = 9,      /* MA_RAMP_OVERLAY */
  Dodge = 10,       /* MA_RAMP_DODGE */
  Burn = 11,        /* MA_RAMP_BURN */
  Hue = 12,         /* MA_RAMP_HUE */
  Saturation = 13,  /* MA_RAMP_SAT */
  Value = 14,       /* MA_RAMP_VAL */
  Color = 15,       /* MA_RAMP_COLOR */
  SoftLight = 16,   /* MA_RAMP_SOFT */
  LinearLight = 17, /* MA_RAMP_LINEAR */
  Exclusion = 18,   /* MA_RAMP_EXCLUSION */
  /**
   * Combine two tangent-space normal maps, rather than blending their encoded values.
   *
   * Encoded normals are not colours: averaging two of them channel by channel flattens the
   * relief instead of laying one over the other, which is why every layer stack that supports
   * normals has an operation of its own for it. This is the whiteout blend -- the detail map's
   * slope added to the base map's, renormalized -- which is what "overlay"/"add" means for a
   * normal layer.
   *
   * Nothing in a plain Mix chain selects this: a Mix node really does interpolate the encoded
   * values, and reproducing it any other way would make the composite disagree with the render.
   * It exists for the graph shapes that genuinely combine normals.
   */
  NormalCombine = 19,
};

/**
 * What a layer *is*, as opposed to how one was made.
 *
 * Stored as an id-property on the layer's Mix nodes -- on the group's own tree for a group -- the
 * same way #BKE_paint_material_layer_marker_get stores identity. A layer with no marker at all
 * reads as #Paint, which is what a stack authored before this contract, or wired by hand in the
 * Shader Editor, actually is.
 */
enum class PaintMaterialLayerKind : int8_t {
  Paint = 0,
  Fill,
  Material,
  /**
   * A child of a layer: its Mix nodes sit between the layer's base and the layer's own Mix node
   * (spec 18 §4.1).
   */
  Correction,
  /* The enum stays open: a kind this build does not know reads back as #Paint. */
};

/**
 * Which part of a correction layer the row's UI shows: the adjustment it applies, or the mask
 * that limits where it applies. Stored next to the kind on the correction's own Mix nodes.
 */
enum class PaintMaterialCorrectionSection : int8_t { Content = 0, Mask = 1 };

/** What a correction layer applies to the layer it hangs under. */
enum class PaintMaterialCorrectionEffect : int8_t { Paint = 0, Fill };

/** How one channel of one stack row stands; see the spec's invariants I1 and I2. */
enum class PaintMaterialLayerChannelState : int8_t {
  /** No map: the row keeps its Mix and Multiply, its coverage is unlinked and zero. */
  Absent = 0,
  /** A map feeds the row, its coverage comes from the map's alpha or the layer's mask. */
  Enabled,
  /** The map stays on its (muted) node, the coverage is unlinked and zero. */
  Disabled,
};

}  // namespace blender
