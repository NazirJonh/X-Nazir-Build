/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edrend
 *
 * Tests only: the material builder of the shader-bake backend, declared so the editor-render tests
 * can inspect the wiring a shader-bake ask builds without the render (a unit test binary has no
 * GPU context, which the render needs). Nothing outside the backend and its tests is meant to use
 * it.
 */

#include "BKE_paint_layers_shader_bake.hh"

#include "BLI_span.hh"

namespace blender {

struct Image;
struct Main;
struct Material;

namespace ed::render {

/** The temporary material a shader-bake ask builds: the request's tree instanced, the convention
 * sockets (`UV`, `Input`, `Result`, `Alpha`) wired, the parameters bound. The render itself is not
 * declared here: a unit test binary has no GPU context, so the tests stop at the wiring. */
Material *paint_layers_shader_bake_backend_material_build(
    Main &bmain,
    const PaintLayerShaderBakeRequest &request,
    Image *input_image,
    Span<const PaintLayerShaderBakeOutput> outputs);

}  // namespace ed::render
}  // namespace blender
