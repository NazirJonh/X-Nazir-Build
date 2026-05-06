#pragma once

#include "overlay_private.hh"

#include "BLI_hash.hh"
#include "BLI_map.hh"
#include "BLI_bounds_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_paint.hh"
#include "BKE_ccg.hh"

namespace blender::draw::overlay {

struct PlaneData {
  float3 normal;
  float3 point;
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
    /* Простая 64-битная смесь для стабильного ключа. */
    uint64_t h = (uint64_t)(key.qx) * 0x9e3779b97f4a7c15ULL;
    h ^= (uint64_t)(key.qy) * 0xc2b2ae3d27d4eb4fULL + (h << 6) + (h >> 2);
    h ^= (uint64_t)(key.axis + 1) * 0x165667b19e3779f9ULL + (h << 6) + (h >> 2);
    return h;
  }
};

struct ContourSegment {
  float3 a;
  float3 b;
  QuantizedPointKey key_a;
  QuantizedPointKey key_b;
  float length = 0.0f;
  bool used = false;
};

/* Symmetry plane definition */
struct SymmetryPlane {
  float3 normal;
  float3 point;
  float distance;
  int axis;  // 0=X, 1=Y, 2=Z
};

/* Intersection segment between triangle and plane */
struct IntersectionSegment {
  float3 start;
  float3 end;
  bool valid;
  int triangle_id;
};

/* Contour loop - closed or open chain of points */
struct ContourLoop {
  Vector<float3> points;
  Vector<float3> normals;
  bool is_closed;
  float length;
};

/**
 * Main class for symmetry contour rendering
 * Supports sculpt mode, edit mode, and texture paint
 */
class SymmetryContour {
private:
  /* Line buffer for rendering contours */
  LinePrimitiveBuf contour_lines_;
  /* Cached shader pointer for safe rebinds. */
  gpu::Shader *contour_shader_ = nullptr;

  /* Cached contour data */
  Vector<ContourLoop> cached_contours_;
  /* Кэш сегментов по оси и индексу PBVH-узла для инкрементальных обновлений. */
  Map<int, Vector<ContourSegment>> cached_segments_by_axis_[3];
  bool contours_dirty_ = true;
  
  /* State tracking for avoiding unnecessary clears */
  bool prev_enable_contours_ = false;
  const Object *prev_object_ = nullptr;
  int prev_symmetry_flags_ = 0;
  bool contours_drawn_this_frame_ = false;  // Track if contours were already drawn
  
  /* Parameters */
  float line_thickness_ = 20.0f;  // Increased thickness
  float3 line_color_ = float3(1.0f, 1.0f, 0.0f);  // Bright yellow for visibility
  float line_alpha_ = 0.6f;  // Semi-transparent
  bool enable_contours_ = false;
  
  /* CPU-based intersection and extraction */
  Vector<IntersectionSegment> compute_intersections_cpu(
      const Mesh *mesh, const SymmetryPlane &plane);
  
  Vector<ContourLoop> extract_contours(
      const Vector<IntersectionSegment> &segments,
      const SymmetryPlane &plane);
  
  void smooth_contour(ContourLoop &contour, float smoothing_factor);

  /* PBVH-based pipeline */
  PlaneData build_plane_data(int axis, float offset, const float4x4 &obmat);
  bool aabb_intersects_plane(const blender::Bounds<float3> &bounds, const PlaneData &plane) const;
  QuantizedPointKey quantize_point(const float3 &p, const PlaneData &plane) const;
  void append_segment(Vector<ContourSegment> &segments,
                      blender::Set<uint64_t> &segment_hashes,
                      const float3 &p0,
                      const float3 &p1,
                      const PlaneData &plane) const;
  void process_pbvh_mesh(const Span<float3> positions,
                         const Mesh *mesh,
                         const bke::pbvh::MeshNode &node,
                         const PlaneData &plane,
                         Vector<ContourSegment> &segments,
                         blender::Set<uint64_t> &segment_hashes) const;
  void process_pbvh_bmesh(const bke::pbvh::BMeshNode &node,
                          const PlaneData &plane,
                          Vector<ContourSegment> &segments,
                          blender::Set<uint64_t> &segment_hashes) const;
  void process_pbvh_grids(const SubdivCCG &subdiv_ccg,
                          const CCGKey &key,
                          const bke::pbvh::GridsNode &node,
                          const PlaneData &plane,
                          Vector<ContourSegment> &segments,
                          blender::Set<uint64_t> &segment_hashes) const;
  void build_contours_from_segments(const Vector<ContourSegment> &segments,
                                    const PlaneData &plane,
                                    Vector<ContourLoop> &r_contours) const;
  void smooth_contour_limited(ContourLoop &contour, const PlaneData &plane) const;
  uint64_t segment_hash(const ContourSegment &seg) const;
  
public:
  SymmetryContour(SelectionType selection_type) : contour_lines_(selection_type, "contour_lines_") {}
  
  /* Initialize contour rendering system */
  void begin_sync(Resources &res, const State &state);
  
  /* Update contours for given object and symmetry settings */
  void update_contours(const Object *ob, int symmetry_flags, const State &state);
  
  /* Finalize contour data for rendering */
  void end_sync(PassSimple::Sub &pass);
  
  /* Force contour recalculation */
  void mark_dirty() { contours_dirty_ = true; }
  
  /* Configuration */
  void set_line_thickness(float thickness) { line_thickness_ = thickness; }
  void set_line_color(const float3 &color) { line_color_ = color; }
  void set_enabled(bool enabled) { enable_contours_ = enabled; }
};

int symmetry_flags_from_mesh_symmetry(char mesh_symmetry);
int symmetry_flags_from_curves_symmetry(char curves_symmetry);

class SymmetryContourOverlay {
 private:
  bool show_ = false;
  PassSimple pass_;
  PassSimple::Sub *sub_ = nullptr;
  SymmetryContour contour_;

 public:
  SymmetryContourOverlay(SelectionType selection_type, const char *pass_name);

  void begin_sync(Resources &res, const State &state, bool show);
  void object_sync(const Object *ob, int symmetry_flags, const State &state);
  void end_sync();
  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view);
};

}  // namespace blender::draw::overlay
