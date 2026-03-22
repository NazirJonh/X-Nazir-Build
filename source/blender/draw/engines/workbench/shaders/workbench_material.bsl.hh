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
                         float vertex_alpha,
                         float3 &color,
                         float &alpha,
                         float &roughness,
                         float &metallic,
                         const float4 channel_mask,
                         const int grayscale)
  {
    float4 data = materials_data[handle];

    /* Count active RGB channels */
    float active_channels = channel_mask.r + channel_mask.g + channel_mask.b;
    bool is_single_channel = (active_channels <= 1.0f);

    /* Alpha channel is special - it's always grayscale regardless of toggle.
     * Only activate when RGB channels are disabled (alpha-only mode). */
    bool is_alpha_only_mode = (channel_mask.a > 0.5f) && (active_channels < 0.5f);

    /* Check if vertex has actual paint data:
     * Unpainted vertices have default white color (1, 1, 1) for RGB
     * Unpainted alpha = 1.0 */
    bool is_unpainted = (vertex_color.r > 0.999f && vertex_color.g > 0.999f && vertex_color.b > 0.999f);

    /* For alpha channel: always grayscale, independent of toggle */
    if (is_alpha_only_mode) {
      /* Alpha channel always uses grayscale mode.
       * Use deviation from white (same as RGB grayscale) since vertex_alpha
       * is typically always 1.0 in vertex data. */
      float3 deviation = float3(1.0f) - vertex_color;
      float brightness = max(deviation.r, max(deviation.g, deviation.b));
      color = (data.r == -1) ? float3(brightness) : data.rgb;
      alpha = 1.0f;
      roughness = 0.5f;
      metallic = 0.0f;
      return;
    }

    if (is_unpainted && is_single_channel) {
      /* Single channel mode: unpainted vertices show as black */
      color = (data.r == -1) ? float3(0.0f) : data.rgb;
      alpha = 1.0f;
      roughness = 0.5f;
      metallic = 0.0f;
      return;
    }

    /* Apply channel mask to vertex color */
    float3 masked_vertex_color = vertex_color * channel_mask.rgb;

    /* Grayscale mode: ONLY when exactly ONE channel is active AND grayscale toggle is on */
    bool use_grayscale = (grayscale != 0) && is_single_channel;

    /* For single channel display:
     * - Show the VALUE of the active channel directly
     * - Unpainted (1,1,1) is handled separately (shows black)
     * - R channel shows R value, G shows G value, B shows B value */
    float3 masked_value = vertex_color * channel_mask.rgb;
    float channel_value = max(masked_value.r, max(masked_value.g, masked_value.b));

    if (use_grayscale) {
      /* Grayscale mode: brightness = channel value (0=black, 1=white) */
      color = (data.r == -1) ? float3(channel_value) : data.rgb;
    }
    else if (is_single_channel) {
      /* Single channel color mode: show color with value for half-tones */
      color = (data.r == -1) ? masked_vertex_color * channel_value : data.rgb;
    }
    else {
      /* Multi-channel color mode: show actual color (old behavior) */
      color = (data.r == -1) ? masked_vertex_color : data.rgb;
    }

    uint encoded_data = floatBitsToUint(data.w);
    alpha = float((encoded_data >> 16u) & 0xFFu) * (1.0f / 255.0f);

    /* Apply alpha channel mask */
    if (data.r == -1 && channel_mask.a < 0.5f) {
      alpha = 1.0f;  /* If A channel is disabled, show as fully opaque */
    }

    roughness = float((encoded_data >> 8u) & 0xFFu) * (1.0f / 255.0f);
    metallic = float(encoded_data & 0xFFu) * (1.0f / 255.0f);
  }
};

}  // namespace workbench::color
