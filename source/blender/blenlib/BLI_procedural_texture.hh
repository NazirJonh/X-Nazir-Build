/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup BLI
 *
 * The scalar math of the procedural textures the shader nodes generate, moved out of the nodes'
 * multi-functions so that a second consumer -- the Paint Layers CPU composite -- reads the same
 * statements the generated shader tree does. A formula copied into the composite is a formula that
 * can drift from the node, and the two then disagree texel by texel; the Checker, Brick and Wave
 * patterns live here for that reason.
 *
 * The enumerations repeat the codes of #eNodeWave_Type and its friends, because this library does
 * not read DNA. The node files hold the `static_assert` that ties the two spellings of each code
 * together, so a renumbering of a node enum fails a build rather than silently changing what a
 * stored parameter means.
 */

#pragma once

#include "BLI_math_vector_types.hh"

namespace blender {

/** The Wave texture's two kinds of pattern, the #SHD_WAVE_BANDS and #SHD_WAVE_RINGS codes. */
enum eProceduralWaveType : int {
  PROC_WAVE_BANDS = 0,
  PROC_WAVE_RINGS = 1,
};

/** Which way the bands run, the #SHD_WAVE_BANDS_DIRECTION_* codes. */
enum eProceduralWaveBandsDirection : int {
  PROC_WAVE_BANDS_DIRECTION_X = 0,
  PROC_WAVE_BANDS_DIRECTION_Y = 1,
  PROC_WAVE_BANDS_DIRECTION_Z = 2,
  PROC_WAVE_BANDS_DIRECTION_DIAGONAL = 3,
};

/** Where the rings are centred, the #SHD_WAVE_RINGS_DIRECTION_* codes. */
enum eProceduralWaveRingsDirection : int {
  PROC_WAVE_RINGS_DIRECTION_X = 0,
  PROC_WAVE_RINGS_DIRECTION_Y = 1,
  PROC_WAVE_RINGS_DIRECTION_Z = 2,
  PROC_WAVE_RINGS_DIRECTION_SPHERICAL = 3,
};

/** The shape of one cycle of the pattern, the #SHD_WAVE_PROFILE_* codes. */
enum eProceduralWaveProfile : int {
  PROC_WAVE_PROFILE_SIN = 0,
  PROC_WAVE_PROFILE_SAW = 1,
  PROC_WAVE_PROFILE_TRI = 2,
};

/**
 * The Checker texture's factor: 1.0 where the cell is the first colour, 0.0 where it is the second,
 * exactly the body of the node's multi-function (including the nudge that keeps a unit coordinate
 * off the cell edge).
 */
float procedural_checker_factor(float3 vector, float scale);

/**
 * The Brick texture's tint noise: the integer hash the node reads for the per-brick colour
 * variation, in 0..2 rather than 0..1 (the caller clamps it with its bias).
 */
float procedural_brick_noise(uint n);

/**
 * The Brick texture's per-texel answer: the tint of the brick under \a p and how much mortar covers
 * it, the node's own two values. \a p is already scaled.
 */
float2 procedural_brick(float3 p,
                        float mortar_size,
                        float mortar_smooth,
                        float bias,
                        float brick_width,
                        float row_height,
                        float offset_amount,
                        int offset_frequency,
                        float squash_amount,
                        int squash_frequency);

/**
 * The Brick texture as a single 0..1 factor: what the node's Color socket shows when its colours
 * are the extremes of the range -- Color1 white, Color2 and Mortar black -- which is the form a
 * generator row reads, because its own colours come from the row's ramp rather than from the node.
 */
float procedural_brick_factor(float3 vector,
                              float scale,
                              float mortar_size,
                              float mortar_smooth,
                              float bias,
                              float brick_width,
                              float row_height,
                              float offset_amount,
                              int offset_frequency,
                              float squash_amount,
                              int squash_frequency);

/**
 * The Wave texture's factor: the coordinate's distance along the bands (or from the rings' centre),
 * displaced by the distortion's noise, read through the profile. Every argument is the node's own
 * input of the same name, and the codes are the #eProceduralWave* values above.
 */
float procedural_wave_factor(float3 vector,
                             float scale,
                             float distortion,
                             float detail,
                             float detail_scale,
                             float detail_roughness,
                             float phase_offset,
                             eProceduralWaveType wave_type,
                             int bands_direction,
                             int rings_direction,
                             eProceduralWaveProfile wave_profile);

}  // namespace blender
