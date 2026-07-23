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

#include "BLI_bounds_types.hh"
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

/** Where one symmetry plane sits in an object's local space. */
struct SymmetryPlanePlacement {
  float3 origin;
  /** Unit in-plane directions spanning the plane. */
  float3 tangent;
  float3 bitangent;
};

/**
 * Placement of the `axis` (0 = X, 1 = Y, 2 = Z) symmetry plane in an object's local space.
 *
 * `symmetry_space_to_object` maps the space the planes are defined in into that object's local
 * space; null means the object's own local axes through its own origin. Shared by the contour and
 * the plane overlays so both agree on where the plane is.
 */
SymmetryPlanePlacement symmetry_plane_placement(int axis,
                                                const float4x4 *symmetry_space_to_object);

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
  /**
   * Everything retained between frames for one object. Keyed by object in #object_caches_ rather
   * than held as a single slot, so a multi-object sculpt (where several objects sync every frame)
   * keeps each object's incremental cache instead of thrashing one shared slot.
   */
  struct ObjectCache {
    /** Last computed contour in object space, re-emitted cheaply while nothing changes. */
    Vector<ContourLoop> contours;
    /**
     * Object-space bounds #contours was built for. Retained so the occlusion bias can be
     * re-derived from the current object-to-world matrix on the cheap re-emit path, where the
     * geometry is unchanged but the transform may not be.
     *
     * Qualified: this namespace also holds the #Bounds overlay (overlay_bounds.hh), which hides
     * the #blender::Bounds template wherever that header is included first.
     */
    blender::Bounds<float3> bounds = {float3(0.0f), float3(0.0f)};
    /** Per-axis, per-PBVH-node segment cache enabling incremental updates while sculpting. */
    Map<int, Vector<ContourSegment>> segments_by_axis[3];
    bool contours_dirty = true;
    /**
     * A full rebuild is in progress and is being spread across frames (see the recompute time
     * budget in #update_contours). Set when a #RegenDecision.reset_cache build starts and cleared
     * once a frame finishes with no dirty work left, so the initial fill of a heavy mesh can use a
     * larger per-frame budget than a live incremental stroke without freezing on a single frame.
     */
    bool filling = false;

    int symmetry_flags = 0;
    /**
     * The plane placement #contours was built for, so a change of symmetry space (multi-object
     * sculpt, see #update_contours) invalidates the cache just like a change of axis flags.
     */
    float4x4 symmetry_space_to_object = float4x4::identity();
    /**
     * Leaf-node count of the last processed PBVH. Detects topology rebuilds (e.g. a Multires
     * display/sculpt level change) that keep the same #Object and don't mark any node's position
     * dirty, which would otherwise leave #segments_by_axis pointing at stale node indices.
     */
    int pbvh_nodes_num = -1;
    /**
     * Last #bke::pbvh::Tree::positions_changed_count() the contour was built for. Detects position
     * edits whose transient dirty-node mask was already consumed before the overlay runs - most
     * notably undo/redo, which tags the nodes and immediately calls #update_bounds (clearing the
     * mask) within the same operator.
     */
    int64_t positions_count = -1;
    /** #frame_counter_ of the last sync, used by #end_sync to drop no-longer-drawn objects. */
    int64_t last_seen_frame = -1;
  };

  /** Accumulated world-space line geometry for every object synced this frame. */
  LinePrimitiveBuf contour_lines_;
  gpu::Shader *contour_shader_ = nullptr;

  Map<const Object *, ObjectCache> object_caches_;
  /** Incremented once per #begin_sync; only used to identify the current frame's syncs. */
  int64_t frame_counter_ = 0;

  /**
   * Navigation fast path. While the object, its transform, the cached object-space contour and the
   * line color all stay put (e.g. orbiting the camera), the world-space #contour_lines_ buffer is
   * identical frame to frame, so it is retained on the GPU - neither re-emitted on the CPU nor
   * re-uploaded. Engaged only for a single synced object; any frame that syncs a different object
   * count falls back to a full re-emit. See #update_contours / #end_sync.
   */
  bool lines_valid_ = false;
  bool buffer_unchanged_this_frame_ = false;
  bool color_changed_ = false;
  int frame_object_count_ = 0;
  int emitted_object_count_ = 0;
  const Object *emitted_object_ = nullptr;
  float4x4 emitted_object_to_world_ = float4x4::identity();
  float4 emitted_color_ = float4(0.0f);

  bool prev_enabled_ = false;

  float line_thickness_ = 5.0f;
  float3 line_color_ = float3(1.0f, 1.0f, 0.0f);
  float line_alpha_ = 1.0f;
  bool enabled_ = false;

  /**
   * Outcome of the per-frame change detection: whether anything moved since the last build and,
   * if so, how much of the cache survives. Carries the fresh #ObjectCache.pbvh_nodes_num /
   * #ObjectCache.positions_count values back so #update_contours can store them after the rebuild.
   */
  struct RegenDecision {
    bool need_regenerate;
    /** The symmetry plane itself moved (axis flags or symmetry space): recompute every node. */
    bool plane_changed;
    /** The whole per-node segment cache is stale and must be dropped before rebuilding. */
    bool reset_cache;
    int pbvh_nodes_num;
    int64_t positions_count;
  };
  /** Decide whether (and how much of) the contour needs rebuilding this frame. */
  RegenDecision compute_regen_decision(const Object *ob,
                                       const ObjectCache &cache,
                                       int symmetry_flags,
                                       const float4x4 &symmetry_space_to_object,
                                       const bke::pbvh::Tree *pbvh,
                                       bool has_dirty_nodes) const;

  /**
   * Transform an object-space contour to world space and append it to the line buffer.
   * `depth_bias` is baked per-vertex rather than pushed as a uniform: the buffer accumulates every
   * synced object into a single draw, so a shared uniform would apply the last object's bias to
   * all of them.
   */
  void emit_loop(const ContourLoop &loop, const float4x4 &object_to_world, float depth_bias);

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
   *
   * `symmetry_space_to_object` maps the space the symmetry planes are defined in into `ob`'s local
   * space. Null means the planes are `ob`'s own local axes through its own origin, which is the
   * case everywhere except a multi-object sculpt stroke, where every mesh mirrors across one plane
   * shared by the whole stroke (see #Sculpts::object_sync).
   */
  void update_contours(const Object *ob,
                       int symmetry_flags,
                       const State &state,
                       BMesh *edit_bm = nullptr,
                       const float4x4 *symmetry_space_to_object = nullptr);
  void end_sync(PassSimple::Sub &pass);

  /**
   * Drop every retained CPU and GPU buffer. Called when the overlay is switched off: the per-node
   * segment cache of a heavy sculpt is worth tens of megabytes and would otherwise stay resident
   * for the rest of the session.
   */
  void release();

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
  /**
   * Whether nothing is currently retained: the caches are freed and the pass is empty. Lets
   * #begin_sync do its teardown once, on the on-to-off transition, instead of every frame the
   * overlay stays off.
   */
  bool released_ = true;
  /**
   * Set for the overlay layer holding "In Front" objects. Those are depth-tested against
   * `depth_in_front_tx` rather than the regular scene depth, which does not contain them.
   */
  bool in_front_ = false;
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
  SymmetryContourOverlay(SelectionType selection_type, const char *pass_name, bool in_front);

  void begin_sync(Resources &res, const State &state, bool show);
  /** See #SymmetryContour::update_contours for `edit_bm` / `symmetry_space_to_object`. */
  void object_sync(const Object *ob,
                   int symmetry_flags,
                   const State &state,
                   BMesh *edit_bm = nullptr,
                   const float4x4 *symmetry_space_to_object = nullptr);
  void end_sync();
  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view);
};

}  // namespace blender::draw::overlay
