/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef QUADRIFLOW_CAPI_HPP
#define QUADRIFLOW_CAPI_HPP

#ifdef __cplusplus
extern "C" {
#endif

struct QuadriflowRemeshData {
  const float *verts;
  const int *faces;
  int totfaces;
  int totverts;

  float *out_verts;
  int *out_faces;
  int out_totverts;
  int out_totfaces;

  int target_faces;
  bool preserve_sharp;
  bool preserve_boundary;
  bool adaptive_scale;
  bool minimum_cost_flow;
  bool aggresive_sat;
  int rng_seed;

  /* Optional per-vertex orientation guidance, indexed by input vertex.
   * Used to bias the cross field so the output follows a desired edge flow
   * (e.g. surface curvature or user guide curves).
   * `guide_dirs` is `totverts * 3` floats, a direction in object space.
   * `guide_weights` is `totverts` floats in [0, 1]; 0 means "no constraint".
   * Both null (the default) leaves the remesher behavior unchanged. */
  const float *guide_dirs;
  const float *guide_weights;
  /* Optional per-vertex positional pins (`totverts` floats in [0, 1]), for
   * vertices on feature lines (e.g. face set boundaries) that the output
   * geometry must pass through. A pinned vertex still slides along its
   * `guide_dirs` direction, matching how `preserve_boundary` keeps open
   * boundaries in place. Null (the default) disables pinning. */
  const float *guide_pin_weights;
  /* Optional per-vertex relative edge-length multipliers (`totverts` floats),
   * driving adaptive quad density: values < 1 produce smaller quads (more
   * detail), values > 1 larger quads. 1 (or <= 0 entries) is neutral. Null
   * (the default) keeps the density uniform. Independent from the guide
   * arrays above. */
  const float *guide_scales;
};

void QFLOW_quadriflow_remesh(QuadriflowRemeshData *qrd,
                             void (*update_cb)(void *, float progress, int *cancel),
                             void *update_cb_data);

#ifdef __cplusplus
}
#endif

#endif  // QUADRIFLOW_CAPI_HPP
