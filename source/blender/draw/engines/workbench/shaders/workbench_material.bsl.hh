/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

namespace workbench::color {

struct Materials {
  [[storage(WB_MATERIAL_SLOT, read)]] float4 (&materials_data)[];

  void material_data_get(int handle,
                         float3 vertex_color,
                         float3 &color,
                         float &alpha,
                         float &roughness,
                         float &metallic,
                         const float4 channel_mask,
                         const int grayscale)
  {
    float4 data = materials_data[handle];

    /* Number of enabled RGB channels. */
    float active_channels = channel_mask.r + channel_mask.g + channel_mask.b;
    bool is_single_channel = (active_channels <= 1.0f);

    /* Alpha-only mode: every RGB channel is disabled and only alpha is enabled.
     * Alpha is always shown as grayscale. Since vertex data alpha is typically 1.0, the
     * deviation of the color from white is used as the displayed value instead. */
    bool is_alpha_only_mode = (channel_mask.a > 0.5f) && (active_channels < 0.5f);
    if (is_alpha_only_mode) {
      float3 deviation = float3(1.0f) - vertex_color;
      float brightness = max(deviation.r, max(deviation.g, deviation.b));
      color = (data.r == -1) ? float3(brightness) : data.rgb;
      alpha = 1.0f;
      roughness = 0.5f;
      metallic = 0.0f;
      return;
    }

    /* Vertex color with the disabled channels masked out. */
    float3 masked_vertex_color = vertex_color * channel_mask.rgb;

    if (is_single_channel && grayscale != 0) {
      /* Grayscale mode: display the value of the single active channel (0 = black, 1 = white). */
      float channel_value = max(masked_vertex_color.r,
                                max(masked_vertex_color.g, masked_vertex_color.b));
      color = (data.r == -1) ? float3(channel_value) : data.rgb;
    }
    else if (is_single_channel) {
      /* Single channel color mode: modulate the channel color by its own value. */
      float channel_value = max(masked_vertex_color.r,
                                max(masked_vertex_color.g, masked_vertex_color.b));
      color = (data.r == -1) ? masked_vertex_color * channel_value : data.rgb;
    }
    else {
      /* Multi-channel color mode: display the masked vertex color directly. */
      color = (data.r == -1) ? masked_vertex_color : data.rgb;
    }

    uint encoded_data = floatBitsToUint(data.w);
    alpha = float((encoded_data >> 16u) & 0xFFu) * (1.0f / 255.0f);

    /* When the alpha channel is disabled, display fully opaque. */
    if (data.r == -1 && channel_mask.a < 0.5f) {
      alpha = 1.0f;
    }

    roughness = float((encoded_data >> 8u) & 0xFFu) * (1.0f / 255.0f);
    metallic = float(encoded_data & 0xFFu) * (1.0f / 255.0f);
  }
};

}  // namespace workbench::color
