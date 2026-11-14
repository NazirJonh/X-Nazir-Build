/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cmath>
#include <memory>

#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_decal_map.hh"

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

struct ColorSpace;
struct ImBuf;

namespace blender {

struct Mesh;
struct Object;
struct PaintMaterialCompositeMapping;
struct PaintMaterialDecalInverseMap;
struct PaintMaterialDecalMap;

/**
 * A sampler over an acquired source buffer, decoded exactly like #composite_resample_mapped's
 * taps: byte storage is straight and decoded to scene-linear, float storage is premultiplied and
 * straightened for the filter, and the result is premultiplied again.
 */
struct CompositeProjectionSampler {
  const ImBuf *src = nullptr;
  bool src_is_float = false;
  int src_channels = 4;
  const ColorSpace *colorspace = nullptr;
  bool decode = false;
  bool decode_by_table = false;
  /**
   * A one-channel float buffer reads as a grey, the convention #composite_resample_mapped and the
   * tile decode read a mask by. The projected bake keeps its red-only read.
   */
  bool grey_one_channel = false;

  explicit CompositeProjectionSampler(const ImBuf *source, bool grey_single_channel = false);

  bool valid() const;

  void tap_straight(int tx, int ty, float r_tap[4]) const;

  /** Bilinear, premultiplied scene-linear tap. False when the coordinate is clipped away. */
  bool sample(float u, float v, bool repeat_u, bool repeat_v, float r_rgba[4]) const;
};

/** The straight colour and alpha of a premultiplied tap. */
void composite_straight_from_premul(const float premul[4], float r_rgb[3], float &r_alpha);

/** Whether \a step (an input transform) acts in its own Decal frame rather than in UV. */
inline bool composite_mapping_step_is_decal(const PaintMaterialCompositeMapping &step)
{
  return step.projection.present && step.projection.space == MA_PAINT_LAYER_MAPPING_SPACE_DECAL;
}

/**
 * The canvas-UV transform of a row's mapping, the one both CPU passes (the resample of a map and
 * the clip of a Material row's coverage) read through, so they cannot disagree on a texel: the
 * Mirror fold of the mapped axes around the center, then rotate around Z, then scale along the
 * canvas axes, then offset. An axis that repeats wraps with floor; one that clips is range-tested.
 */
struct CompositeMappingTransform {
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float cos_rot = 1.0f;
  float sin_rot = 0.0f;
  float offset_u = 0.0f;
  float offset_v = 0.0f;
  bool mirror_u = false;
  bool mirror_v = false;
  /* The fold keeps the smaller side of the center instead of the larger one. */
  bool mirror_negative_u = false;
  bool mirror_negative_v = false;
  float mirror_center_u = 0.5f;
  float mirror_center_v = 0.5f;
  bool repeat_u = true;
  bool repeat_v = true;

  /* The identity; only the unused slots of #CompositeMappingChain::steps hold it. */
  CompositeMappingTransform() = default;

  explicit CompositeMappingTransform(const PaintMaterialCompositeMapping &mapping)
      : scale_x(BKE_paint_layers_mapping_scale_normalize(mapping.scale[0])),
        scale_y(BKE_paint_layers_mapping_scale_normalize(mapping.scale[1])),
        cos_rot(cosf(mapping.rotation)),
        sin_rot(sinf(mapping.rotation)),
        offset_u(mapping.offset[0]),
        offset_v(mapping.offset[1]),
        mirror_u(mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U)),
        mirror_v(mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V)),
        mirror_negative_u(mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U) &&
                          mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U_NEGATIVE)),
        mirror_negative_v(mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V) &&
                          mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V_NEGATIVE)),
        mirror_center_u(mapping.mirror_center[0]),
        mirror_center_v(mapping.mirror_center[1])
  {
    BKE_paint_layers_mapping_wrap_axes(mapping.wrap, repeat_u, repeat_v);
  }

  /** Whether any axis clips, so a texel can fall outside the unit square. */
  bool clips() const
  {
    return !(repeat_u && repeat_v);
  }

  /** Which side of the fold center \a u lies on along U, when U folds; always +1 when it does not.
   * The covariant counterpart of the fold, the flip a tangent normal's U component takes. */
  float side_u(const float u) const
  {
    return mirror_u ? BKE_paint_layers_mapping_mirror_side(u, mirror_center_u, mirror_negative_u) :
                      1.0f;
  }

  /** #side_u along V. */
  float side_v(const float v) const
  {
    return mirror_v ? BKE_paint_layers_mapping_mirror_side(v, mirror_center_v, mirror_negative_v) :
                      1.0f;
  }

  /** The mapped coordinate of the canvas point (\a u, \a v), with no repeat: the chain floors the
   * composed lookup at its end, and a step whose axis clips only reports the texel transparent.
   * \return false outside the square on an axis that clips. */
  bool apply_fold_affine(const float u, const float v, float &r_u, float &r_v) const
  {
    float fu = u;
    float fv = v;
    if (mirror_u) {
      fu = BKE_paint_layers_mapping_mirror_fold(u, mirror_center_u, mirror_negative_u);
    }
    if (mirror_v) {
      fv = BKE_paint_layers_mapping_mirror_fold(v, mirror_center_v, mirror_negative_v);
    }
    const float ru = fu * cos_rot - fv * sin_rot;
    const float rv = fu * sin_rot + fv * cos_rot;
    r_u = ru * scale_x + offset_u;
    r_v = rv * scale_y + offset_v;
    return !((!repeat_u && (r_u < 0.0f || r_u >= 1.0f)) ||
             (!repeat_v && (r_v < 0.0f || r_v >= 1.0f)));
  }

  /** The mapped coordinate of the canvas point (\a u, \a v). \return false outside the square on
   * an axis that clips. Only a clipping axis is range-tested: a wrapped one can round to 1. The
   * fold reads the canvas UV the painting addresses, before the transform moves it. */
  bool apply(const float u, const float v, float &r_u, float &r_v) const
  {
    if (!apply_fold_affine(u, v, r_u, r_v)) {
      return false;
    }
    if (repeat_u) {
      r_u -= floorf(r_u);
    }
    if (repeat_v) {
      r_v -= floorf(r_v);
    }
    return true;
  }
};

/**
 * The composed read of a row's own map: the transform effects' steps first, in the effects list's
 * order, then the row's own mapping, the order the generated graph composes the same effects in.
 * Every step folds about its center and then transforms; an axis a step clips makes the texel
 * transparent, and the composed lookup repeats at the end, the way the graph's Image Texture
 * extension repeats the composed lookup it is fed (a clipping step has already declared the texel
 * transparent, so flooring an in-range coordinate is the identity it reads as).
 */
struct CompositeMappingChain {
  /** The most steps one read composes (#PAINT_MATERIAL_COMPOSITE_INPUT_TRANSFORM_MAX transform
   * effects plus the row's own mapping). The description leaves the extras out. */
  static constexpr int MAX_STEPS = PAINT_MATERIAL_COMPOSITE_INPUT_TRANSFORM_MAX + 1;

  CompositeMappingTransform steps[MAX_STEPS];
  int steps_num = 0;
  /** The composed map holds tangent-space normals: the read also remaps the vectors. */
  bool tangent_normal = false;

  bool is_identity() const
  {
    return steps_num == 0;
  }

  /** Whether any step clips an axis, so a texel can fall outside the square. */
  bool clips() const
  {
    for (int i = 0; i < steps_num; i++) {
      if (steps[i].clips()) {
        return true;
      }
    }
    return false;
  }

  /**
   * The composed lookup of the canvas point (\a u, \a v). \return false when a step's clipping
   * axis puts the texel outside the square. \a r_step_sides, when given, receives the fold's side
   * sign per step at the coordinate that enters it, for the tangent-Normal remap below.
   */
  bool apply(const float u,
             const float v,
             float &r_u,
             float &r_v,
             float (*r_step_sides)[2] = nullptr) const
  {
    float cu;
    float cv;
    if (!this->apply_unwrapped(u, v, cu, cv, r_step_sides)) {
      return false;
    }
    r_u = cu - floorf(cu);
    r_v = cv - floorf(cv);
    return true;
  }

  /**
   * #apply without the closing repeat: the steps compose and the caller wraps (or clips) the result.
   * A projected read wants this, since the row's own UV adjust and wrap act after the steps.
   * \return false when a step's clipping axis puts the texel outside the square.
   */
  bool apply_unwrapped(const float u,
                       const float v,
                       float &r_u,
                       float &r_v,
                       float (*r_step_sides)[2] = nullptr) const
  {
    float cu = u;
    float cv = v;
    for (int i = 0; i < steps_num; i++) {
      const CompositeMappingTransform &step = steps[i];
      if (r_step_sides != nullptr) {
        /* The side signs are per-texel, taken where the step's fold acts: on the coordinate the
         * step reads, which is the previous step's output. */
        r_step_sides[i][0] = step.side_u(cu);
        r_step_sides[i][1] = step.side_v(cv);
      }
      if (!step.apply_fold_affine(cu, cv, cu, cv)) {
        return false;
      }
    }
    r_u = cu;
    r_v = cv;
    return true;
  }

  /**
   * The covariant counterpart of the composed read, for an encoded tangent normal: the inverse
   * Jacobian of the chain, applied from the last step to the first, each step's side signs the
   * ones \a step_sides recorded for this texel. The magnitudes do not turn a direction, only the
   * signs of the scales do.
   */
  void remap_tangent_normal(const float (*step_sides)[2], float r_xy[2]) const
  {
    float nx = r_xy[0];
    float ny = r_xy[1];
    for (int i = steps_num - 1; i >= 0; i--) {
      const CompositeMappingTransform &step = steps[i];
      const float fx = (step.scale_x < 0.0f ? -1.0f : 1.0f) * nx;
      const float fy = (step.scale_y < 0.0f ? -1.0f : 1.0f) * ny;
      /* The UV transform of a step is `S * R * fold`, whose inverse on a direction is
       * `side * R^T * S^-1`, the fold's side signs acting on the canvas axes after the rotation
       * back. Composing the steps, the last one's inverse acts first. */
      nx = step_sides[i][0] * (step.cos_rot * fx + step.sin_rot * fy);
      ny = step_sides[i][1] * (-step.sin_rot * fx + step.cos_rot * fy);
    }
    r_xy[0] = nx;
    r_xy[1] = ny;
  }
};

/**
 * The chain a map read goes through: \a row's own mapping last, the transform effects' steps first
 * in \a input_steps' order, only the enabled ones. A Decal-space step has no UV form and is left
 * out; `composite_resample_effect_space` reads it.
 */
CompositeMappingChain composite_mapping_chain_build(
    const PaintMaterialCompositeMapping &mapping,
    Span<PaintMaterialCompositeMapping> input_steps);

/**
 * Resample \a src onto a \a ref_width x \a ref_height grid through \a chain, as a float RGBA
 * premultiplied scene-linear #ImBuf the caller owns. Null when the source cannot be read.
 */
ImBuf *composite_resample_mapped(const ImBuf *src,
                                 int ref_width,
                                 int ref_height,
                                 const CompositeMappingChain &chain);

/**
 * The mesh a projected row bakes through: the evaluated mesh when \a object is an evaluated object,
 * so the bake sees the modifiers' UV and positions the 3D viewport shows, else the original data.
 * Null when \a object is not a mesh object or has no mesh.
 */
const Mesh *composite_projection_mesh(const Object *object);

/**
 * A hash of \a mesh's positions, topology, render UV, material indices and corner normals. The
 * heavy content hash is cached per mesh and re-run only when a cheap fingerprint (array pointers,
 * counts, a strided sample) moves or the cached value is a second old, so calling it every frame
 * costs a few thousand reads.
 */
uint64_t composite_mesh_content_hash(const Mesh &mesh);

/** A premultiplied scene-linear transparent buffer of \a width by \a height, or null. */
ImBuf *composite_alloc_transparent(int width, int height);

/**
 * Bake \a src into the painted object's UV space through the projection of \a mapping. Null when
 * the mapping names no object, mesh or render UV map.
 *
 * \a input_steps are the row's transform effects (Transform Mapping, Mirror), in the effects list's
 * order. They act on the map coordinate the projection computes, before the row's own UV adjust;
 * a clipping wrap on a step makes the texel transparent outside that coordinate's unit square. The
 * Cube mode runs them in the local [0, 1] space of the tap's own atlas cell.
 */
ImBuf *composite_resample_projected(const ImBuf *src,
                                    int ref_width,
                                    int ref_height,
                                    const PaintMaterialCompositeMapping &mapping,
                                    Span<PaintMaterialCompositeMapping> input_steps);

/* -------------------------------------------------------------------- */
/** \name Effect Space
 *
 * Transform effects whose step acts in a Decal frame of their own (#composite_mapping_step_is_decal)
 * on a row that reads its map at the surface UV. See paint_material_composite_effect_space.cc.
 * \{ */

/** One step of an effect-space chain: a fold and transform, acting in UV or in the Decal frame. */
struct CompositeEffectSpaceStep {
  CompositeMappingTransform transform;
  /** The step acts on `c = 0.5 + 0.5 q` of the Decal frame instead of on the UV. */
  bool decal = false;
};

/**
 * The composed read of a row whose steps mix spaces. The current coordinate is a UV or a map
 * coordinate `q`; a step of the other space converts it first, through the forward map (UV to `q`)
 * or the inverse map (`q` to UV). Consecutive Decal steps compose over `q` with no conversion in
 * between, and the read ends in UV.
 */
struct CompositeEffectSpaceChain {
  static constexpr int MAX_STEPS = CompositeMappingChain::MAX_STEPS;
  /** The most events one texel records: every step, and one conversion on each side of it. */
  static constexpr int MAX_EVENTS = MAX_STEPS * 3;

  CompositeEffectSpaceStep steps[MAX_STEPS];
  int steps_num = 0;
  /** The forward and inverse maps of the row's one Decal frame; both set when any step is Decal. */
  const PaintMaterialDecalMap *decal_map = nullptr;
  const PaintMaterialDecalInverseMap *inverse_map = nullptr;
  /** The frame's #MA_PAINT_LAYER_PROJECTION_CLIP: a texel the patch does not reach is transparent
   * rather than read without the Decal steps. */
  bool clip_outside_patch = false;
  bool tangent_normal = false;

  bool has_decal() const
  {
    for (int i = 0; i < steps_num; i++) {
      if (steps[i].decal) {
        return true;
      }
    }
    return false;
  }
};

/** What one texel's resolve did, in order, for the tangent-Normal remap. */
struct CompositeEffectSpaceEvent {
  enum class Type : int8_t {
    /** A step's fold and transform; #side holds its fold sides. */
    Step,
    /** UV to `q` on patch triangle #tri (the destination side). */
    EnterDecal,
    /** `q` to UV on patch triangle #tri (the source side). */
    ExitDecal,
  };
  Type type = Type::Step;
  int8_t step = 0;
  int tri = -1;
  float side[2] = {1.0f, 1.0f};
};

struct CompositeEffectSpaceTrace {
  CompositeEffectSpaceEvent events[CompositeEffectSpaceChain::MAX_EVENTS];
  int events_num = 0;
};

enum class CompositeEffectSpaceResult : int8_t {
  /** The source UV was found. */
  Mapped,
  /** A clipping axis or the patch clip leaves the texel transparent. */
  Transparent,
  /** The patch does not reach a point a Decal step needs (and the patch does not clip). */
  OutsidePatch,
};

/**
 * Resolve the source UV (tile-local, not yet wrapped) the destination texel at tile UV \a uv reads,
 * \a texel_index its index in the forward map (-1 when \a uv is no texel centre of it). Pure data,
 * so the tests run it without an Object or an ImBuf.
 */
CompositeEffectSpaceResult composite_effect_space_resolve(const CompositeEffectSpaceChain &chain,
                                                          const float2 &uv,
                                                          int64_t texel_index,
                                                          float2 &r_uv,
                                                          CompositeEffectSpaceTrace *r_trace);

/**
 * The resolve of every destination texel of one chain: the source UV and the result kind. It does
 * not depend on the row's map, so the channels of one layer (same chain, same frame) share it and
 * only sample their own map. The tangent-Normal trace is not stored: a Normal channel resolves
 * with a trace and skips the field.
 */
struct CompositeEffectSpaceField {
  int width = 0;
  int height = 0;
  /** Valid where #status is #CompositeEffectSpaceResult::Mapped. */
  Array<float2> src_uv;
  Array<uint8_t> status;
  /** Everything that moved the resolve; compared exactly, so a stale field is never served. */
  PaintMaterialDecalMapKey map_key;
  Vector<float> key_values;

  size_t bytes() const
  {
    return size_t(this->src_uv.size()) * sizeof(float2) + size_t(this->status.size());
  }
};

/**
 * The cached #CompositeEffectSpaceField of \a chain on a \a width by \a height grid, built on a miss
 * outside the cache's lock: channels resolving in parallel wait for one build rather than repeat it.
 * A small LRU (two entries, a memory cap): one 4K field is 9 bytes per texel, so the cache holds a
 * bake's and a preview's.
 * \a r_hit tells a cached field; \a r_build_seconds the time spent building it.
 */
std::shared_ptr<const CompositeEffectSpaceField> composite_effect_space_field_get(
    const CompositeEffectSpaceChain &chain,
    int width,
    int height,
    bool &r_hit,
    double &r_build_seconds);

/**
 * Drop the cached fields. The cache is a global whose entries hold guarded memory, so it has to be
 * emptied before the allocator's exit check: a static destructor would free after it is gone.
 */
void composite_effect_space_field_cache_free();

/**
 * Carry the decoded tangent-space normal \a r_normal read at the source back to the destination
 * along \a trace: each step's covariant inverse (the UV path's), and at each conversion the turn
 * from the source triangle's tangent basis into the unfolded plane and from the plane into the
 * destination triangle's tangent basis.
 */
void composite_effect_space_remap_normal(const CompositeEffectSpaceChain &chain,
                                         const CompositeEffectSpaceTrace &trace,
                                         float3 &r_normal);

/**
 * Resample \a src onto the tile's \a ref_width x \a ref_height grid through \a input_steps and the
 * row's own \a mapping, when some step is a Decal step. Null when the Decal frame cannot be built
 * (no object, mesh or render UV, no seed); the caller then reads the steps in UV without the Decal
 * ones.
 */
ImBuf *composite_resample_effect_space(const ImBuf *src,
                                       int ref_width,
                                       int ref_height,
                                       const PaintMaterialCompositeMapping &mapping,
                                       Span<PaintMaterialCompositeMapping> input_steps);

/** \} */

}  // namespace blender
