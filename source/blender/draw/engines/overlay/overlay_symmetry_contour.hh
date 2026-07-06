/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 *
 * Symmetry overlays.
 *
 * #SymmetryContour computes and draws the contour where a mesh surface intersects the X/Y/Z
 * symmetry planes. It supports every mesh editing context: sculpt mode (using the paint BVH with
 * incremental per-node caching for fast live updates), edit mode and the paint modes (weight,
 * vertex, texture). The resulting line geometry is generated in world space and accumulated for
 * all synced objects into a single line buffer.
 *
 * Curves objects, which have no surface to contour, are handled by #SymmetryPlaneOverlay (see
 * overlay_symmetry_plane.hh) which draws the translucent symmetry planes instead.
 */

#pragma once

#include "overlay_private.hh"

#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

struct Object;
struct Mesh;
struct BMesh;

namespace blender::bke::pbvh {
class Tree;
}

namespace blender::draw::overlay {

/** Convert a mesh `ME_SYMMETRY_*` bit-field into `PAINT_SYMM_*` axis flags. */
int symmetry_flags_from_mesh_symmetry(char mesh_symmetry);
/** Convert a curves `CURVES_SYMMETRY_*` bit-field into `PAINT_SYMM_*` axis flags. */
int symmetry_flags_from_curves_symmetry(char curves_symmetry);

/**
 * A single symmetry plane plus the tolerances used while extracting its contour.
 * The tolerances are derived from the object size at build time so the result is scale
 * independent.
 */
struct PlaneParams {
  float3 normal;
  float3 point;
  /** In-plane basis used to quantize intersection points into a 2D grid for welding. */
  float3 tangent;
  float3 bitangent;
  int axis = 0;

  float quant_step = 1e-4f;
  float min_seg_len = 1e-4f;
  float min_loop_len = 1e-3f;
  float plane_tolerance = 1e-5f;

  float smooth_factor = 0.5f;
  int smooth_iters = 3;
  float smooth_max_disp = 0.0f;
};

/** Quantized 2D position in a plane's local basis, used to weld coincident endpoints. */
struct QuantizedPointKey {
  int qx;
  int qy;
  int axis;

  bool operator==(const QuantizedPointKey &other) const
  {
    return qx == other.qx && qy == other.qy && axis == other.axis;
  }
};

struct QuantizedPointKeyHash {
  uint64_t operator()(const QuantizedPointKey &key) const
  {
    uint64_t h = uint64_t(key.qx) * 0x9e3779b97f4a7c15ULL;
    h ^= uint64_t(key.qy) * 0xc2b2ae3d27d4eb4fULL + (h << 6) + (h >> 2);
    h ^= uint64_t(key.axis + 1) * 0x165667b19e3779f9ULL + (h << 6) + (h >> 2);
    return h;
  }
};

/** A single triangle/plane intersection segment in object space. */
struct ContourSegment {
  float3 a;
  float3 b;
  QuantizedPointKey key_a;
  QuantizedPointKey key_b;
  float length = 0.0f;
};

/** An ordered chain of points forming one contour line in object space. */
struct ContourLoop {
  Vector<float3> points;
  bool is_closed = false;
  float length = 0.0f;
};

/**
 * Computes and renders the symmetry contour for a single overlay (one per editing context).
 * The class is geometry-source agnostic: #update_contours dispatches to the matching extraction
 * path based on the object data and mode, and all paths feed the same contour builder.
 */
class SymmetryContour {
 private:
  /** Accumulated world-space line geometry for every object synced this frame. */
  LinePrimitiveBuf contour_lines_;
  gpu::Shader *contour_shader_ = nullptr;

  /** Last computed contour in object space, re-emitted cheaply while nothing changes. */
  Vector<ContourLoop> cached_contours_;
  /** Per-axis, per-PBVH-node segment cache enabling incremental updates while sculpting. */
  Map<int, Vector<ContourSegment>> cached_segments_by_axis_[3];
  bool contours_dirty_ = true;

  const Object *prev_object_ = nullptr;
  int prev_symmetry_flags_ = 0;
  bool prev_enabled_ = false;
  /**
   * Leaf-node count of the last processed PBVH. Detects topology rebuilds (e.g. a Multires
   * display/sculpt level change) that keep the same #Object and don't mark any node's position
   * dirty, which would otherwise leave #cached_segments_by_axis_ pointing at stale node indices.
   */
  int prev_pbvh_nodes_num_ = -1;
  /**
   * Last #bke::pbvh::Tree::positions_changed_count() the contour was built for. Detects position
   * edits whose transient dirty-node mask was already consumed before the overlay runs - most
   * notably undo/redo, which tags the nodes and immediately calls #update_bounds (clearing the
   * mask) within the same operator.
   */
  int64_t prev_positions_count_ = -1;

  float line_thickness_ = 5.0f;
  float3 line_color_ = float3(1.0f, 1.0f, 0.0f);
  float line_alpha_ = 1.0f;
  /** World-space occlusion tolerance, derived from the object size (see #update_contours). */
  float depth_bias_ = 0.0f;
  bool enabled_ = false;

  /**
   * Outcome of the per-frame change detection: whether anything moved since the last build and,
   * if so, how much of the cache survives. Carries the fresh #prev_pbvh_nodes_num_ /
   * #prev_positions_count_ values back so #update_contours can store them after the rebuild.
   */
  struct RegenDecision {
    bool need_regenerate;
    /** Object or symmetry flags changed: every node must be recomputed from scratch. */
    bool object_changed;
    /** The whole per-node segment cache is stale and must be dropped before rebuilding. */
    bool reset_cache;
    int pbvh_nodes_num;
    int64_t positions_count;
  };
  /** Decide whether (and how much of) the contour needs rebuilding this frame. */
  RegenDecision compute_regen_decision(const Object *ob,
                                       int symmetry_flags,
                                       const bke::pbvh::Tree *pbvh,
                                       bool has_dirty_nodes) const;

  /** Transform an object-space contour to world space and append it to the line buffer. */
  void emit_loop(const ContourLoop &loop, const float4x4 &object_to_world);

 public:
  SymmetryContour(SelectionType selection_type)
      : contour_lines_(selection_type, "symmetry_contour_lines")
  {
  }

  void begin_sync(Resources &res, const State &state);
  /**
   * `edit_bm` must be non-null when `ob` is in Edit Mode: the `Mesh` used for drawing carries no
   * populated position/face arrays there (the live geometry lives in the `BMesh` instead), so the
   * caller resolves it (it already has the `BKE_editmesh.hh` include needed for the full type).
   */
  void update_contours(const Object *ob,
                       int symmetry_flags,
                       const State &state,
                       BMesh *edit_bm = nullptr);
  void end_sync(PassSimple::Sub &pass);

  void mark_dirty()
  {
    contours_dirty_ = true;
  }
  void set_enabled(bool enabled)
  {
    enabled_ = enabled;
  }
};

/**
 * Pass wrapper around #SymmetryContour. Owns the render pass and is embedded in the per-mode
 * overlay classes (Meshes, Paints, Sculpts).
 */
class SymmetryContourOverlay {
 private:
  bool show_ = false;
  PassSimple pass_;
  PassSimple::Sub *sub_ = nullptr;
  SymmetryContour contour_;
  /**
   * Kept from #begin_sync so #draw_line can target the depth-less line frame-buffer and bind the
   * scene depth for the occlusion test (the regular line frame-buffer has a depth attachment and
   * therefore cannot also be sampled as a texture).
   */
  Resources *res_ = nullptr;

 public:
  SymmetryContourOverlay(SelectionType selection_type, const char *pass_name);

  void begin_sync(Resources &res, const State &state, bool show);
  void object_sync(const Object *ob,
                   int symmetry_flags,
                   const State &state,
                   BMesh *edit_bm = nullptr);
  void end_sync();
  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view);
};

}  // namespace blender::draw::overlay
