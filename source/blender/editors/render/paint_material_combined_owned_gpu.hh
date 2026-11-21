/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edrend
 *
 * The owned-effect GPU provider: the editors' registration of the combined preview's GPU effect
 * pipeline as #BKE_paint_layers_owned_effect_gpu_provider_set's answer. A bake of an effect that
 * owns its result asks the provider before it recomputes the effect on the CPU, and the provider
 * computes it the way the preview does -- the same described input, the same pass plan, the same
 * GPU result cache -- and reads the result back into a float #ImBuf with the CPU effect buffer's
 * semantics. The changed rectangle the bake hands in (the subscription's view of the sources it
 * edited) only narrows the pass dispatches the stage makes; the answer itself is always the effect's
 * full, fresh result. Every refusal answers null, which the bake answers with the CPU compute.
 *
 * The only thing a caller needs from this file is the register below; the provider itself lives in
 * paint_material_combined_owned_gpu.cc, and the module's second hook -- the mask-effect provider of
 * paint_material_combined_mask_gpu.cc -- registers through it as well.
 */

namespace blender::ed::material_combined {

/**
 * Register the owned-effect GPU provider with BKE. Idempotent: every call sets the same provider
 * function again, so a caller may invoke it once at the start of the bake path that computes owned
 * effects, on the main thread and before any compute asks for the hook. Registration costs nothing
 * and makes no GPU calls -- the provider enables the draw GPU context and ensures the compute
 * shaders itself, the way #combined_row_render_gpu does, when the first effect asks it.
 *
 * Un-registering is not needed: the provider refuses on its own terms (a material set to the CPU
 * composite, the `BLENDER_PBR_GPU_COMBINED=0` comparison switch, no GPU context) and BKE falls
 * back to the CPU compute on a null answer. The register also sets the mask-effect provider of
 * paint_material_combined_mask_gpu.cc (#paint_material_combined_mask_gpu_register), the module's
 * second hook, under the same terms.
 */
void paint_material_combined_owned_gpu_register();

/**
 * Register the mask-effect GPU provider with BKE -- a module-internal half of
 * #paint_material_combined_owned_gpu_register, not a caller's own entry point. The provider itself
 * lives in paint_material_combined_mask_gpu.cc.
 */
void paint_material_combined_mask_gpu_register();

}  // namespace blender::ed::material_combined
