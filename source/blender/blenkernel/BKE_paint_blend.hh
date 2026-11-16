/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The CPU statement of the paint blend codes, keyed on the ramp-code space the Mix node and the
 * extended `.PBR Blend` modes share. It lives apart from the material composite so that a second
 * paint stack may reuse it without including the material's headers. The implementation still
 * owns the one DNA dependency this needs: the #eMaterialPaintLayerBlend numbering that
 * #BKE_paint_blend_to_ramp translates from.
 */

#include <cstdint>

namespace blender {

/**
 * Blend codes past the Mix node's own range: the stack modes a single `ShaderNodeMix` cannot
 * express. They continue the #eMaterial_RampBlend numbering, because the two share one code space
 * -- the paint composite's blend, the ".PBR Blend" group's `Mode` input and the CPU's
 * #BKE_paint_blend_apply all read these numbers -- but they are not Mix blend types: no Mix node
 * may hold one, which is why the modes of this range are built out of the group's own nodes.
 * 19 is the normal combine, which has a group of its own and never reaches the blend chain.
 *
 * Runtime-only, which is why this is a BKE header and not the DNA one: no struct field stores
 * these codes, and the file format names the same modes through #eMaterialPaintLayerBlend, whose
 * own numbering runs past them by one (#MA_PAINT_LAYER_BLEND_NORMAL_REPLACE at 20). The enum lives
 * here, not in the material composite header, so the blend arithmetic can name the codes without
 * including that header.
 */
enum eMaterialPaintBlend : int {
  MA_PAINT_BLEND_HARD_LIGHT = 20,
  MA_PAINT_BLEND_VIVID_LIGHT = 21,
  MA_PAINT_BLEND_PIN_LIGHT = 22,
  MA_PAINT_BLEND_LINEAR_BURN = 23,
  MA_PAINT_BLEND_LIGHTER_COLOR = 24,
  MA_PAINT_BLEND_DARKER_COLOR = 25,
};

/**
 * Blend \a col over \a r_col by \a fac, for one paint blend code: the CPU statement of what the
 * ".PBR Blend" group's node clusters compute on the GPU.
 *
 * A #MA_RAMP_* code is the Mix node's own `ramp_blend`; a code past its range is the extended
 * arithmetic that range needs. The factor is clamped to 0..1 by the caller's conventions
 * (the row blend clamps it; nothing else clamps, so a float buffer keeps a mode's overshoot).
 * Unknown codes leave \a r_col alone.
 */
void BKE_paint_blend_apply(int code, float r_col[4], float fac, const float col[4]);

/**
 * The ramp code (MA_RAMP_* or #eMaterialPaintBlend) an #eMaterialPaintLayerBlend value stands for.
 * The normal combine slot has no single answer, so \a normal_combine_code supplies it: the
 * material's Normal channel builds a plain Mix (MA_RAMP_BLEND). An unknown blend answers
 * MA_RAMP_BLEND.
 */
int BKE_paint_blend_to_ramp(int blend, int normal_combine_code);

}  // namespace blender
