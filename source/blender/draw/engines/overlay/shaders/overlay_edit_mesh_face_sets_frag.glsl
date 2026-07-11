/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_edit_mesh_face_sets)

void main()
{
  /* Output the face set color prepared by the vertex shader. The blending mode (BLEND_MUL or
   * BLEND_ALPHA) is selected by the render pipeline based on the retopology state. */
  frag_color = face_set_color;
}
