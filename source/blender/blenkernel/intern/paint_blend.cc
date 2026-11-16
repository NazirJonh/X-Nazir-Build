/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The implementation of #BKE_paint_blend_apply, the shared CPU arithmetic of the paint blend
 * codes: see BKE_paint_blend.hh for the contract. `DNA_material_types.h` owns the ramp codes and
 * the #eMaterialPaintLayerBlend numbering that #BKE_paint_blend_to_ramp translates from.
 */

#include "BKE_paint_blend.hh"

#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_color_blend.h"

#include "BKE_material.hh"

#include "DNA_material_types.h"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Paint blend codes
 *
 * The CPU arithmetic of the paint blend codes: every `MA_RAMP_*` mode a Mix node expresses
 * goes through #ramp_blend, the six #eMaterialPaintBlend modes past its range are computed here.
 * The ".PBR Blend" group builds each of them from nodes the Mix node already has (see
 * #BKE_paint_material_blend_group_ensure); the formulas below are what those node clusters reduce
 * to, and the parity tests in `paint_layers_graph_eval_test.cc` hold the two together.
 * \{ */

/**
 * The whole-colour result of a mode a Mix node has no blend type for.
 *
 * Each returns the unweighted blend of \a below and \a above, the Photoshop definition with no
 * factor in it: the caller mixes the answer back over \a below by its own factor, which is how the
 * ".PBR Blend" group builds the same mode -- a cluster for the formula, then one Mix by the factor.
 * The two engines must agree channel for channel, and neither of them clamps: the byte encode is
 * where a result finally comes back inside 0..1.
 */
static float paint_blend_extended_channel(const int type, const float below, const float above)
{
  switch (type) {
    case MA_PAINT_BLEND_HARD_LIGHT:
      /* Overlay with the two colours exchanged: the layer decides which half of the curve. */
      return (above < 0.5f) ? (2.0f * below * above) :
                              (1.0f - 2.0f * (1.0f - below) * (1.0f - above));
    case MA_PAINT_BLEND_LINEAR_BURN:
      return below + above - 1.0f;
    case MA_PAINT_BLEND_VIVID_LIGHT:
      /* One of the two ramps the Mix node already computes, against the layer pushed to twice its
       * value or to twice its value less one -- and it is the node's own form of each that both
       * engines have to agree on, not the textbook one: #blend_color_burn_channel and
       * #blend_color_dodge_channel are the factor-one cases of #ramp_blend's Burn and Dodge. */
      return (above < 0.5f) ? blend_color_burn_channel(below, 2.0f * above) :
                              blend_color_dodge_channel(below, 2.0f * (1.0f - above));
    case MA_PAINT_BLEND_PIN_LIGHT:
      return (above < 0.5f) ? min_ff(below, 2.0f * above) : max_ff(below, 2.0f * above - 1.0f);
    default:
      return below;
  }
}

/** The RGB-sum decision of Lighter/Darker Color: does \a above win over \a below? */
static bool paint_blend_color_sum_wins(const int type, const float below[3], const float above[3])
{
  const float sum_below = below[0] + below[1] + below[2];
  const float sum_above = above[0] + above[1] + above[2];
  return (type == MA_PAINT_BLEND_LIGHTER_COLOR) ? (sum_above > sum_below) : (sum_above < sum_below);
}

void BKE_paint_blend_apply(int code, float r_col[4], float fac, const float col[4])
{
  if (code <= MA_RAMP_EXCLUSION) {
    /* A Mix node mode: the node's own arithmetic is the one statement of it. */
    ramp_blend(code, r_col, fac, col);
    return;
  }
  switch (code) {
    case MA_PAINT_BLEND_HARD_LIGHT:
    case MA_PAINT_BLEND_VIVID_LIGHT:
    case MA_PAINT_BLEND_PIN_LIGHT:
    case MA_PAINT_BLEND_LINEAR_BURN: {
      const float full[3] = {
          paint_blend_extended_channel(code, r_col[0], col[0]),
          paint_blend_extended_channel(code, r_col[1], col[1]),
          paint_blend_extended_channel(code, r_col[2], col[2]),
      };
      for (const int i : IndexRange(3)) {
        r_col[i] = (1.0f - fac) * r_col[i] + fac * full[i];
      }
      break;
    }
    case MA_PAINT_BLEND_LIGHTER_COLOR:
    case MA_PAINT_BLEND_DARKER_COLOR: {
      /* One whole colour wins, so the decision is made once and the factor mixes the winner in. */
      const bool above_wins = paint_blend_color_sum_wins(code, r_col, col);
      const float full[3] = {above_wins ? col[0] : r_col[0],
                             above_wins ? col[1] : r_col[1],
                             above_wins ? col[2] : r_col[2]};
      for (const int i : IndexRange(3)) {
        r_col[i] = (1.0f - fac) * r_col[i] + fac * full[i];
      }
      break;
    }
    default:
      /* An unknown code blends nothing: the caller clamps its modes at the description, so this
       * only ever reads a corrupt value, and leaving the colour alone is the safe answer. */
      break;
  }
}

/**
 * The ramp code an #eMaterialPaintLayerBlend value maps to. The normal combine slot is left to the
 * caller, because the material's Normal channel builds a plain Mix for it while another stack may
 * want the normal-combine code.
 */
int BKE_paint_blend_to_ramp(const int blend, const int normal_combine_code)
{
  switch (eMaterialPaintLayerBlend(blend)) {
    case MA_PAINT_LAYER_BLEND_MIX:
      return MA_RAMP_BLEND;
    case MA_PAINT_LAYER_BLEND_MULTIPLY:
      return MA_RAMP_MULT;
    case MA_PAINT_LAYER_BLEND_OVERLAY:
      return MA_RAMP_OVERLAY;
    case MA_PAINT_LAYER_BLEND_ADD:
      return MA_RAMP_ADD;
    case MA_PAINT_LAYER_BLEND_DARKEN:
      return MA_RAMP_DARK;
    case MA_PAINT_LAYER_BLEND_BURN:
      return MA_RAMP_BURN;
    case MA_PAINT_LAYER_BLEND_LIGHTEN:
      return MA_RAMP_LIGHT;
    case MA_PAINT_LAYER_BLEND_SCREEN:
      return MA_RAMP_SCREEN;
    case MA_PAINT_LAYER_BLEND_DODGE:
      return MA_RAMP_DODGE;
    case MA_PAINT_LAYER_BLEND_SUBTRACT:
      return MA_RAMP_SUB;
    case MA_PAINT_LAYER_BLEND_DIVIDE:
      return MA_RAMP_DIV;
    case MA_PAINT_LAYER_BLEND_DIFFERENCE:
      return MA_RAMP_DIFF;
    case MA_PAINT_LAYER_BLEND_EXCLUSION:
      return MA_RAMP_EXCLUSION;
    case MA_PAINT_LAYER_BLEND_SOFT_LIGHT:
      return MA_RAMP_SOFT;
    case MA_PAINT_LAYER_BLEND_LINEAR_LIGHT:
      return MA_RAMP_LINEAR;
    case MA_PAINT_LAYER_BLEND_HUE:
      return MA_RAMP_HUE;
    case MA_PAINT_LAYER_BLEND_SATURATION:
      return MA_RAMP_SAT;
    case MA_PAINT_LAYER_BLEND_COLOR:
      return MA_RAMP_COLOR;
    case MA_PAINT_LAYER_BLEND_VALUE:
      return MA_RAMP_VAL;
    case MA_PAINT_LAYER_BLEND_NORMAL_COMBINE:
      return normal_combine_code;
    case MA_PAINT_LAYER_BLEND_NORMAL_REPLACE:
      /* The Normal channel builds a plain Mix for Replace; the code is never read there. */
      return MA_RAMP_BLEND;
    case MA_PAINT_LAYER_BLEND_HARD_LIGHT:
      return MA_PAINT_BLEND_HARD_LIGHT;
    case MA_PAINT_LAYER_BLEND_VIVID_LIGHT:
      return MA_PAINT_BLEND_VIVID_LIGHT;
    case MA_PAINT_LAYER_BLEND_PIN_LIGHT:
      return MA_PAINT_BLEND_PIN_LIGHT;
    case MA_PAINT_LAYER_BLEND_LINEAR_BURN:
      return MA_PAINT_BLEND_LINEAR_BURN;
    case MA_PAINT_LAYER_BLEND_LIGHTER_COLOR:
      return MA_PAINT_BLEND_LIGHTER_COLOR;
    case MA_PAINT_LAYER_BLEND_DARKER_COLOR:
      return MA_PAINT_BLEND_DARKER_COLOR;
  }
  return MA_RAMP_BLEND;
}

/** \} */

}  // namespace blender
