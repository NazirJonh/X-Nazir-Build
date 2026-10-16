/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 *
 * Draw engine to draw the Image/UV editor
 */

#include "ED_material_combined.hh"

#include "image_engine.h"
#include "image_instance.hh"
#include "image_shader.hh"

namespace blender::image_engine {

DrawEngine *Engine::create_instance()
{
  return new Instance();
}

void Engine::free_static()
{
  ShaderModule::module_free();
  ed::material_combined::combined_preview_gpu_free();
}

}  // namespace blender::image_engine
