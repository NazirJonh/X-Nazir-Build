/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void compose_color_alpha(float4 color, float alpha, float4 &result)
{
  result = float4(color.rgb, alpha);
}
