#include "overlay_symmetry_contour.hh"
#include "overlay_private.hh"

#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_paint.hh"
#include "BKE_ccg.hh"
#include "DEG_depsgraph_query.hh"
#include "BLI_bit_vector.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_bounds.hh"
#include "BLI_task.hh"
#include "BLI_rect.h"
#include "bmesh.hh"
#include "GPU_batch.hh"
#include "GPU_batch_presets.hh"
#include "GPU_index_buffer.hh"
#include "GPU_vertex_buffer.hh"
#include "GPU_vertex_format.hh"

#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>

#include <atomic>
#include <mutex>
#include <unordered_map>

#include "DNA_screen_types.h"
#include "DNA_mesh_types.h"
#include "DNA_curves_types.h"

namespace blender::draw::overlay {

/* Вспомогательный флаг для локальной отладки. */
static constexpr bool contour_debug = true;
static constexpr uint32_t contour_debug_period = 120;

struct ContourDebugLogEntry {
  uint64_t call_count = 0;
  uint64_t last_signature = 0;
  uint32_t suppressed = 0;
  bool has_last_signature = false;
};

static bool contour_debug_should_log(const int line,
                                     const uint64_t signature,
                                     const bool use_signature,
                                     uint32_t &r_suppressed)
{
  static std::mutex mutex;
  static std::unordered_map<int, ContourDebugLogEntry> entries;

  std::scoped_lock lock(mutex);
  ContourDebugLogEntry &entry = entries[line];

  const bool first = (entry.call_count == 0);
  const bool changed = use_signature && (!entry.has_last_signature || entry.last_signature != signature);
  const bool periodic = (entry.call_count % contour_debug_period) == 0;
  const bool should_log = first || changed || periodic;

  r_suppressed = entry.suppressed;
  if (should_log) {
    entry.suppressed = 0;
  }
  else {
    entry.suppressed++;
  }

  if (use_signature) {
    entry.last_signature = signature;
    entry.has_last_signature = true;
  }
  entry.call_count++;
  return should_log;
}

static void contour_debug_log_impl(const int line,
                                   const uint64_t signature,
                                   const bool use_signature,
                                   const char *fmt,
                                   ...)
{
  if (!contour_debug) {
    return;
  }

  uint32_t suppressed = 0;
  if (!contour_debug_should_log(line, signature, use_signature, suppressed)) {
    return;
  }

  if (suppressed > 0) {
    printf("[CONTOUR_DEBUG] (line %d) suppressed %u repeated messages\n", line, suppressed);
  }

  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
}

#define CONTOUR_LOG(...) contour_debug_log_impl(__LINE__, 0, false, __VA_ARGS__)
#define CONTOUR_LOG_STATE(signature, ...) \
  contour_debug_log_impl(__LINE__, uint64_t(signature), true, __VA_ARGS__)

static int contour_debug_marker = []() {
  printf("[CONTOUR_DEBUG] overlay_symmetry_contour.cc loaded\n");
  return 0;
}();

static bool intersect_triangle_plane(const float3 &v0,
                                     const float3 &v1,
                                     const float3 &v2,
                                     const SymmetryPlane &plane,
                                     float3 &intersection_start,
                                     float3 &intersection_end);

/* Построение плоскостей усечённой пирамиды вида для отсечения PBVH-узлов. */
static int build_frustum_planes(const RegionView3D *rv3d, float r_planes[6][4])
{
  if (rv3d == nullptr) {
    return 0;
  }
  planes_from_projmat(
      rv3d->persmat, r_planes[0], r_planes[1], r_planes[2], r_planes[3], r_planes[4], r_planes[5]);
  return 6;
}

/* Проекция точки в экранные координаты. Возвращает false, если за камерой или w≈0. */
static bool project_point_to_screen(const float3 &world_pos,
                                    const float4x4 &persmat,
                                    const int2 region_size,
                                    float2 &r_screen)
{
  const float4 clip = persmat * float4(world_pos, 1.0f);
  if (math::abs(clip.w) < 1e-8f || clip.w < 0.0f) {
    return false;
  }
  const float3 ndc = float3(clip) / clip.w;
  if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z)) {
    return false;
  }
  /* NDC [-1,1] -> screen [0,w],[0,h] */
  r_screen.x = (ndc.x * 0.5f + 0.5f) * region_size.x;
  r_screen.y = (ndc.y * 0.5f + 0.5f) * region_size.y;
  return true;
}

/* Прореживание ломаной в экранном пространстве по порогу pixel_step. */
static void decimate_contour_screen(ContourLoop &contour,
                                    const float4x4 &persmat,
                                    const int2 region_size,
                                    const float pixel_step)
{
  if (contour.points.size() < 2 || pixel_step <= 0.0f) {
    return;
  }

  Vector<float3> decimated;
  decimated.reserve(contour.points.size());

  float2 prev_screen;
  bool prev_valid = project_point_to_screen(contour.points[0], persmat, region_size, prev_screen);
  decimated.append(contour.points[0]);
  float accum = 0.0f;

  for (int i = 1; i < contour.points.size(); i++) {
    float2 curr_screen;
    bool valid = project_point_to_screen(contour.points[i], persmat, region_size, curr_screen);
    if (!prev_valid && !valid) {
      continue;
    }

    float seg_len = 0.0f;
    if (prev_valid && valid) {
      seg_len = math::length(curr_screen - prev_screen);
    }
    accum += seg_len;

    if (accum >= pixel_step || (!valid && prev_valid)) {
      decimated.append(contour.points[i]);
      accum = 0.0f;
    }

    prev_screen = curr_screen;
    prev_valid = valid;
  }

  if (!decimated.is_empty() && decimated.last() != contour.points.last()) {
    decimated.append(contour.points.last());
  }

  if (decimated.size() >= 2) {
    contour.points = decimated;
  }
}

PlaneData SymmetryContour::build_plane_data(int axis, float offset, const float4x4 &obmat)
{
  (void)obmat; /* TODO: support world offset when UI появится. */
  PlaneData plane;
  plane.axis = axis;
  plane.normal = float3(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f);
  plane.point = plane.normal * offset;

  plane.tangent = (axis == 0) ? float3(0.0f, 1.0f, 0.0f) :
                 (axis == 1) ? float3(0.0f, 0.0f, 1.0f) :
                               float3(1.0f, 0.0f, 0.0f);
  plane.bitangent = math::cross(plane.normal, plane.tangent);
  if (math::length_squared(plane.bitangent) < 1e-8f) {
    plane.bitangent = float3(0.0f, 0.0f, 1.0f);
  }
  plane.tangent = math::normalize(plane.tangent);
  plane.bitangent = math::normalize(plane.bitangent);
  return plane;
}

bool SymmetryContour::aabb_intersects_plane(const blender::Bounds<float3> &bounds,
                                            const PlaneData &plane) const
{
  float3 center = (bounds.min + bounds.max) * 0.5f;
  float3 half = (bounds.max - bounds.min) * 0.5f;
  float dist = math::dot(plane.normal, center - plane.point);
  float radius = math::abs(plane.normal.x) * half.x +
                 math::abs(plane.normal.y) * half.y +
                 math::abs(plane.normal.z) * half.z;
  return math::abs(dist) <= radius + plane.plane_tolerance;
}

uint64_t SymmetryContour::segment_hash(const ContourSegment &seg) const
{
  QuantizedPointKey k0 = seg.key_a;
  QuantizedPointKey k1 = seg.key_b;
  uint64_t h0 = QuantizedPointKeyHash{}(k0);
  uint64_t h1 = QuantizedPointKeyHash{}(k1);
  if (h1 < h0) {
    std::swap(h0, h1);
  }
  return h0 ^ (h1 + 0x9e3779b97f4a7c15ULL + (h0 << 6) + (h0 >> 2));
}

QuantizedPointKey SymmetryContour::quantize_point(const float3 &p, const PlaneData &plane) const
{
  const float2 uv(math::dot(p, plane.tangent), math::dot(p, plane.bitangent));
  const float inv_step = 1.0f / plane.quant_step;
  QuantizedPointKey key;
  key.qx = int(math::round(uv.x * inv_step));
  key.qy = int(math::round(uv.y * inv_step));
  key.axis = plane.axis;
  return key;
}

void SymmetryContour::append_segment(Vector<ContourSegment> &segments,
                                     blender::Set<uint64_t> &segment_hashes,
                                     const float3 &p0,
                                     const float3 &p1,
                                     const PlaneData &plane) const
{
  const float len = math::distance(p0, p1);
  if (len < plane.min_seg_len) {
    return;
  }

  QuantizedPointKey k0 = quantize_point(p0, plane);
  QuantizedPointKey k1 = quantize_point(p1, plane);

  uint64_t h0 = QuantizedPointKeyHash{}(k0);
  uint64_t h1 = QuantizedPointKeyHash{}(k1);
  if (h1 < h0) {
    std::swap(h0, h1);
  }
  const uint64_t seg_hash = h0 ^ (h1 + 0x9e3779b97f4a7c15ULL + (h0 << 6) + (h0 >> 2));
  if (!segment_hashes.add(seg_hash)) {
    return; /* дубликат */
  }

  ContourSegment seg;
  seg.a = p0;
  seg.b = p1;
  seg.key_a = k0;
  seg.key_b = k1;
  seg.length = len;
  segments.append(seg);
}

void SymmetryContour::process_pbvh_mesh(const Span<float3> positions,
                                        const Mesh *mesh,
                                        const bke::pbvh::MeshNode &node,
                                        const PlaneData &plane,
                                        Vector<ContourSegment> &segments,
                                        blender::Set<uint64_t> &segment_hashes) const
{
  if (!mesh) {
    return;
  }
  const OffsetIndices faces = mesh->faces();
  const Span<int> corner_verts = mesh->corner_verts();

  const Span<int> node_faces = node.faces();
  for (const int face_i : node_faces) {
    const IndexRange face = faces[face_i];
    if (face.size() < 3) {
      continue;
    }
    const int v0_idx = corner_verts[face[0]];
    for (int i = 1; i < face.size() - 1; i++) {
      const int v1_idx = corner_verts[face[i]];
      const int v2_idx = corner_verts[face[i + 1]];

      float3 s, e;
      SymmetryPlane plane_cpu;
      plane_cpu.normal = plane.normal;
      plane_cpu.point = plane.point;
      plane_cpu.axis = plane.axis;
      plane_cpu.distance = 0.0f;

      if (intersect_triangle_plane(positions[v0_idx], positions[v1_idx], positions[v2_idx],
                                   plane_cpu, s, e))
      {
        append_segment(segments, segment_hashes, s, e, plane);
      }
    }
  }
}

void SymmetryContour::process_pbvh_bmesh(const bke::pbvh::BMeshNode &node,
                                         const PlaneData &plane,
                                         Vector<ContourSegment> &segments,
                                         blender::Set<uint64_t> &segment_hashes) const
{
  /* Используем triangulated данные, если есть. */
  if (!node.orig_tris_.is_empty() && !node.orig_positions_.is_empty()) {
    for (const int3 &tri : node.orig_tris_) {
      const float3 &v0 = node.orig_positions_[tri.x];
      const float3 &v1 = node.orig_positions_[tri.y];
      const float3 &v2 = node.orig_positions_[tri.z];
      float3 s, e;
      SymmetryPlane plane_cpu{plane.normal, plane.point, 0.0f, plane.axis};
      if (intersect_triangle_plane(v0, v1, v2, plane_cpu, s, e)) {
        append_segment(segments, segment_hashes, s, e, plane);
      }
    }
    return;
  }

  for (BMFace *f : node.bm_faces_) {
    if (f->len < 3) {
      continue;
    }
    BMLoop *l_first = f->l_first;
    float3 v0 = float3(l_first->v->co);
    BMLoop *l = l_first->next;
    for (int i = 1; i < f->len - 1; i++, l = l->next) {
      float3 v1 = float3(l->v->co);
      float3 v2 = float3(l->next->v->co);
      float3 s, e;
      SymmetryPlane plane_cpu{plane.normal, plane.point, 0.0f, plane.axis};
      if (intersect_triangle_plane(v0, v1, v2, plane_cpu, s, e)) {
        append_segment(segments, segment_hashes, s, e, plane);
      }
    }
  }
}

void SymmetryContour::process_pbvh_grids(const SubdivCCG &subdiv_ccg,
                                         const CCGKey &key,
                                         const bke::pbvh::GridsNode &node,
                                         const PlaneData &plane,
                                         Vector<ContourSegment> &segments,
                                         blender::Set<uint64_t> &segment_hashes) const
{
  const Span<float3> positions = subdiv_ccg.positions;
  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
  const int grid_size = key.grid_size;
  
  /* Обходим все грид-квады в узле. */
  for (const int grid : node.grids()) {
    const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
    
    /* Для каждого квада в гриде создаем два треугольника. */
    for (const short y : IndexRange(grid_size - 1)) {
      for (const short x : IndexRange(grid_size - 1)) {
        /* Проверяем, не скрыт ли квад. */
        if (!grid_hidden.is_empty()) {
          if (paint_is_grid_face_hidden(grid_hidden[grid], grid_size, x, y)) {
            continue;
          }
        }
        
        /* Получаем позиции четырех углов квада. */
        const float3 &v00 = grid_positions[CCG_grid_xy_to_index(grid_size, x, y)];
        const float3 &v10 = grid_positions[CCG_grid_xy_to_index(grid_size, x + 1, y)];
        const float3 &v11 = grid_positions[CCG_grid_xy_to_index(grid_size, x + 1, y + 1)];
        const float3 &v01 = grid_positions[CCG_grid_xy_to_index(grid_size, x, y + 1)];
        
        /* Создаем плоскость для пересечения. */
        SymmetryPlane plane_cpu{plane.normal, plane.point, 0.0f, plane.axis};
        
        /* Первый треугольник: v00, v10, v11. */
        float3 s1, e1;
        if (intersect_triangle_plane(v00, v10, v11, plane_cpu, s1, e1)) {
          append_segment(segments, segment_hashes, s1, e1, plane);
        }
        
        /* Второй треугольник: v00, v11, v01. */
        float3 s2, e2;
        if (intersect_triangle_plane(v00, v11, v01, plane_cpu, s2, e2)) {
          append_segment(segments, segment_hashes, s2, e2, plane);
        }
      }
    }
  }
}

void SymmetryContour::smooth_contour_limited(ContourLoop &contour,
                                             const PlaneData &plane) const
{
  if (contour.points.size() < 3) {
    return;
  }

  for (int iter = 0; iter < plane.smooth_iters; iter++) {
    Vector<float3> new_points = contour.points;
    for (int i = 1; i < contour.points.size() - 1; i++) {
      const float3 prev = contour.points[i - 1];
      const float3 next = contour.points[i + 1];
      const float3 cur = contour.points[i];
      const float3 target = (prev + next) * 0.5f;
      float3 delta = (target - cur) * plane.smooth_factor;
      const float delta_len = math::length(delta);
      if (delta_len > plane.smooth_max_disp && delta_len > 0.0f) {
        delta *= plane.smooth_max_disp / delta_len;
      }
      new_points[i] = cur + delta;
    }
    contour.points = new_points;
  }
}

void SymmetryContour::build_contours_from_segments(const Vector<ContourSegment> &segments,
                                                   const PlaneData &plane,
                                                   Vector<ContourLoop> &r_contours) const
{
  if (segments.is_empty()) {
    return;
  }

  blender::Map<uint64_t, Vector<int>> adjacency;
  adjacency.reserve(segments.size() * 2);

  for (const int i : segments.index_range()) {
    const auto &seg = segments[i];
    const uint64_t ha = QuantizedPointKeyHash{}(seg.key_a);
    const uint64_t hb = QuantizedPointKeyHash{}(seg.key_b);
    adjacency.lookup_or_add_default(ha).append(i);
    adjacency.lookup_or_add_default(hb).append(i);
  }

  Vector<bool> used(segments.size(), false);

  for (const int start_idx : segments.index_range()) {
    if (used[start_idx]) {
      continue;
    }
    ContourLoop loop;
    loop.is_closed = false;
    loop.length = 0.0f;

    const ContourSegment *seg = &segments[start_idx];
    used[start_idx] = true;

    loop.points.append(seg->a);
    loop.points.append(seg->b);

    QuantizedPointKey start_key = seg->key_a;
    QuantizedPointKey current_key = seg->key_b;
    uint64_t current_hash = QuantizedPointKeyHash{}(current_key);
    const uint64_t start_hash = QuantizedPointKeyHash{}(start_key);

    while (true) {
      Vector<int> *next_list = adjacency.lookup_ptr(current_hash);
      if (next_list == nullptr) {
        break;
      }
      int next_idx = -1;
      for (const int cand : *next_list) {
        if (!used[cand]) {
          next_idx = cand;
          break;
        }
      }
      if (next_idx == -1) {
        break;
      }

      used[next_idx] = true;
      const ContourSegment &next_seg = segments[next_idx];
      if (next_seg.key_a == current_key) {
        loop.points.append(next_seg.b);
        current_key = next_seg.key_b;
      }
      else {
        loop.points.append(next_seg.a);
        current_key = next_seg.key_a;
      }
      current_hash = QuantizedPointKeyHash{}(current_key);
    }

    loop.length = 0.0f;
    for (int i = 1; i < loop.points.size(); i++) {
      loop.length += math::distance(loop.points[i - 1], loop.points[i]);
    }

    if (current_hash == start_hash && loop.points.size() > 2) {
      loop.is_closed = true;
    }

    if (loop.length < plane.min_loop_len) {
      continue;
    }

    smooth_contour_limited(loop, plane);
    r_contours.append(loop);
  }
}

/* Triangle-plane intersection using existing BLI functions */
static bool intersect_triangle_plane(
    const float3 &v0, const float3 &v1, const float3 &v2,
    const SymmetryPlane &plane,
    float3 &intersection_start, float3 &intersection_end)
{
  // Compute distances from vertices to plane
  float d0 = math::dot(v0 - plane.point, plane.normal);
  float d1 = math::dot(v1 - plane.point, plane.normal);
  float d2 = math::dot(v2 - plane.point, plane.normal);
  
  const float epsilon = 1e-6f;
  
  // Classify vertices relative to plane
  bool s0 = d0 > epsilon;
  bool s1 = d1 > epsilon;
  bool s2 = d2 > epsilon;
  
  int positive_count = int(s0) + int(s1) + int(s2);
  
  // All vertices on same side - no intersection
  if (positive_count == 0 || positive_count == 3) {
    return false;
  }
  
  // Find intersection points on edges
  float3 vertices[3] = {v0, v1, v2};
  float distances[3] = {d0, d1, d2};
  float3 intersection_points[2];
  int intersection_count = 0;
  
  for (int i = 0; i < 3 && intersection_count < 2; i++) {
    int next = (i + 1) % 3;
    
    if ((distances[i] > epsilon && distances[next] < -epsilon) ||
        (distances[i] < -epsilon && distances[next] > epsilon))
    {
      float t = distances[i] / (distances[i] - distances[next]);
      intersection_points[intersection_count++] = 
          math::interpolate(vertices[i], vertices[next], t);
    }
    else if (math::abs(distances[i]) <= epsilon) {
      intersection_points[intersection_count++] = vertices[i];
    }
  }
  
  if (intersection_count == 2) {
    intersection_start = intersection_points[0];
    intersection_end = intersection_points[1];
    return true;
  }
  
  return false;
}

Vector<IntersectionSegment> SymmetryContour::compute_intersections_cpu(
    const Mesh *mesh, const SymmetryPlane &plane)
{
  Vector<IntersectionSegment> segments;
  
  if (!mesh || !mesh->faces_num) {
    return segments;
  }
  
  const Span<float3> positions = mesh->vert_positions();
  const OffsetIndices faces = mesh->faces();
  const Span<int> corner_verts = mesh->corner_verts();
  
  // Process each face
  for (int face_i = 0; face_i < mesh->faces_num; face_i++) {
    const IndexRange face = faces[face_i];
    
    if (face.size() < 3) continue;
    
    // Triangulate face (simple fan triangulation)
    for (int i = 1; i < face.size() - 1; i++) {
      int v0_idx = corner_verts[face[0]];
      int v1_idx = corner_verts[face[i]];
      int v2_idx = corner_verts[face[i + 1]];
      
      float3 intersection_start, intersection_end;
      if (intersect_triangle_plane(
              positions[v0_idx], positions[v1_idx], positions[v2_idx],
              plane, intersection_start, intersection_end))
      {
        IntersectionSegment segment;
        segment.start = intersection_start;
        segment.end = intersection_end;
        segment.valid = true;
        segment.triangle_id = face_i;
        segments.append(segment);
      }
    }
  }
  
  return segments;
}

// Implement contour extraction with KD-tree
Vector<ContourLoop> SymmetryContour::extract_contours(
    const Vector<IntersectionSegment> &segments,
    const SymmetryPlane &plane)
{
  (void)plane; // Suppress unused parameter warning
  Vector<ContourLoop> contours;

  if (segments.is_empty()) {
    return contours;
  }

  // Simplified version without KD-tree: just add each segment as a separate contour
  // This avoids the BLI_kdtree.hh namespace conflict
  for (int i = 0; i < segments.size(); i++) {
    ContourLoop contour;
    contour.is_closed = false;
    contour.length = math::distance(segments[i].start, segments[i].end);
    contour.points.append(segments[i].start);
    contour.points.append(segments[i].end);
    if (contour.points.size() >= 2) {
      contours.append(contour);
    }
  }

  return contours;
}

void SymmetryContour::smooth_contour(ContourLoop &contour, float smoothing_factor)
{
  if (contour.points.size() < 3) return;
  
  Vector<float3> smoothed = contour.points;
  
  // Laplacian smoothing (3 iterations)
  for (int iter = 0; iter < 3; iter++) {
    Vector<float3> new_points = smoothed;
    
    for (int i = 1; i < smoothed.size() - 1; i++) {
      float3 laplacian = (smoothed[i-1] + smoothed[i+1]) * 0.5f - smoothed[i];
      new_points[i] = smoothed[i] + laplacian * smoothing_factor;
    }
    
    smoothed = new_points;
  }
  
  contour.points = smoothed;
}

void SymmetryContour::begin_sync(Resources &res, const State &state)
{
  CONTOUR_LOG("[CONTOUR_DEBUG] begin_sync called\n");
  
  /* Кэшируем шейдер для контура без штриховки. */
  contour_shader_ = res.shaders->extra_wire_contour.get();
  /* TODO(PRJ-FEAT-008): Вернуться к вопросу AA контура.
   * Нужна модель, аналогичная осям: сглаживание через post-AA при глобальном Smooth Wire,
   * или отдельный флаг/настройка для контура. Решить UX и флаг. */

  // Reset draw flag for new frame
  contours_drawn_this_frame_ = false;
  
  // Clear line buffer for new frame
  contour_lines_.clear();

  line_thickness_ = state.overlay.sculpt_symmetry_contour_thickness;
  if (line_thickness_ < 5.0f) {
    line_thickness_ = 5.0f;
  }

  /* Цвет/альфа из темы: новый цвет в теме для контура симметрии. */
  const float4 theme_col = res.theme.colors.sculpt_symmetry_contour;
  line_color_ = float3(theme_col);
  line_alpha_ = theme_col.w;

  const uint64_t contour_param_signature = (uint64_t(int(line_thickness_ * 100.0f)) & 0xFFFFull) |
                                           ((uint64_t(int(line_color_.x * 255.0f)) & 0xFFull)
                                            << 16) |
                                           ((uint64_t(int(line_color_.y * 255.0f)) & 0xFFull)
                                            << 24) |
                                           ((uint64_t(int(line_color_.z * 255.0f)) & 0xFFull)
                                            << 32) |
                                           ((uint64_t(int(line_alpha_ * 255.0f)) & 0xFFull)
                                            << 40);
  CONTOUR_LOG_STATE(contour_param_signature,
                    "[CONTOUR_DEBUG] contour params: thickness=%.2f color=(%.3f %.3f %.3f) alpha=%.3f theme=(%.3f %.3f %.3f %.3f)\n",
              line_thickness_,
              line_color_.x,
              line_color_.y,
              line_color_.z,
              line_alpha_,
              theme_col.x,
              theme_col.y,
              theme_col.z,
              theme_col.w);
}

void SymmetryContour::update_contours(
    const Object *ob, int symmetry_flags, const State &state)
{
  const uint64_t update_signature = (uint64_t(enable_contours_) & 0x1ull) |
                                    ((uint64_t(uint32_t(symmetry_flags)) & 0xFFFFull) << 1) |
                                    ((uint64_t(ob ? ob->mode : 0) & 0xFFFFull) << 17);

  CONTOUR_LOG_STATE(update_signature,
                    "[CONTOUR_DEBUG] update_contours called: enable_contours_=%d, symmetry_flags=%d\n",
              enable_contours_,
              symmetry_flags);
  (void)state;

  CONTOUR_LOG_STATE(update_signature,
                    "[CONTOUR_DEBUG] update_contours invoked: enable=%d ob=%p mode=%d sym_flags=%d\n",
         enable_contours_,
         (void *)ob,
         ob ? int(ob->mode) : -1,
         symmetry_flags);

  const bool object_changed = ob != prev_object_ || symmetry_flags != prev_symmetry_flags_;
  const SculptSession *ss = ob ? ob->runtime->sculpt_session : nullptr;
  const bool stroke_active = (ss && ss->cache); /* Идёт штрих – форсируем обновление. */

  if (!enable_contours_ || !ob || !ob->data) {
    CONTOUR_LOG_STATE(update_signature,
                      "[CONTOUR_DEBUG] Early exit: enable_contours_=%d, ob=%p\n",
                      enable_contours_,
                      (void *)ob);
    return;
  }

  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*const_cast<Object *>(ob));
  IndexMaskMemory dirty_memory;
  IndexMask dirty_nodes;
  bool has_dirty_nodes = false;
  if (pbvh != nullptr) {
    dirty_nodes = bke::pbvh::pbvh_positions_dirty_mask(*pbvh, dirty_memory);
    has_dirty_nodes = !dirty_nodes.is_empty();
    CONTOUR_LOG("[CONTOUR_DEBUG] PBVH type=%d nodes=%d dirty_nodes=%d stroke=%d force_live=%d\n",
                int(pbvh->type()),
                pbvh->nodes_num(),
                has_dirty_nodes ? int(dirty_nodes.size()) : 0,
                int(stroke_active),
                int(ob && ob->mode == OB_MODE_SCULPT));
    const uint64_t pbvh_signature = (uint64_t(pbvh->type()) & 0x7ull) |
                                    ((uint64_t(pbvh->nodes_num()) & 0xFFFFull) << 3) |
                                    ((uint64_t(has_dirty_nodes ? int(dirty_nodes.size()) : 0) &
                                      0xFFFFull)
                                     << 19) |
                                    ((uint64_t(stroke_active) & 0x1ull) << 35);
    CONTOUR_LOG_STATE(pbvh_signature,
                      "[CONTOUR_DEBUG] PBVH type=%d nodes=%d dirty=%d stroke=%d\n",
                      int(pbvh->type()),
                      pbvh->nodes_num(),
                      has_dirty_nodes ? int(dirty_nodes.size()) : 0,
                      int(stroke_active));
  }
  else {
    CONTOUR_LOG("[CONTOUR_DEBUG] PBVH missing, will use full-mesh fallback\n");
  }

  const bool force_live = stroke_active; /* Только во время активного штриха. */
  const bool need_regenerate = contours_dirty_ ||
                               enable_contours_ != prev_enable_contours_ || object_changed ||
                               has_dirty_nodes || stroke_active;

  if (!need_regenerate && !cached_contours_.is_empty()) {
    for (const ContourLoop &contour : cached_contours_) {
      if (contour.points.size() < 2) {
        continue;
      }
      for (int i = 0; i < contour.points.size() - 1; i++) {
        contour_lines_.append(contour.points[i],
                              contour.points[i + 1],
                              float4(line_color_, line_alpha_));
      }
      if (contour.is_closed && contour.points.size() >= 3) {
        contour_lines_.append(contour.points.last(),
                              contour.points.first(),
                              float4(line_color_, line_alpha_));
      }
    }
    return;
  }

  cached_contours_.clear();
  contour_lines_.clear();

  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob);

  /* Берём деформированные координаты (скульптовые), чтобы контур обновлялся на лету.
   * В режимах, где SculptSession/PBVH отсутствуют, используем безопасный фоллбек. */
  Span<float3> eval_positions;
  const bool can_use_pbvh_positions = (pbvh != nullptr) &&
                                      (pbvh->type() == bke::pbvh::Type::Mesh) &&
                                      ob && ob->runtime && ob->runtime->sculpt_session;
  if (can_use_pbvh_positions && ob && !DEG_is_original(ob)) {
    eval_positions = bke::pbvh::vert_positions_eval_from_eval(*ob);
    CONTOUR_LOG("[CONTOUR_DEBUG] positions source: eval_from_eval, count=%d\n",
                int(eval_positions.size()));
  }
  else if (can_use_pbvh_positions && state.depsgraph && ob) {
    eval_positions = bke::pbvh::vert_positions_eval(*state.depsgraph, *ob);
    CONTOUR_LOG("[CONTOUR_DEBUG] positions source: depsgraph+orig, count=%d\n",
                int(eval_positions.size()));
  }
  else {
    /* Фоллбек: текущие позиции меша (eval/оригинал по данным DRW). */
    eval_positions = mesh.vert_positions();
    CONTOUR_LOG("[CONTOUR_DEBUG] positions source: mesh, count=%d\n", int(eval_positions.size()));
  }

  if (eval_positions.is_empty()) {
    CONTOUR_LOG("[CONTOUR_DEBUG] positions empty; skipping contour build\n");
    return;
  }

  /* Оценка масштаба для допусков.
   * Используем габариты PBVH/меша, чтобы не сканировать все вершины (дорого на высокополигональных мешах). */
  float3 bb_min(FLT_MAX);
  float3 bb_max(-FLT_MAX);
  if (pbvh != nullptr) {
    const blender::Bounds<float3> pbvh_bounds = bke::pbvh::bounds_get(*pbvh);
    bb_min = pbvh_bounds.min;
    bb_max = pbvh_bounds.max;
  }
  else if (!eval_positions.is_empty()) {
    for (const float3 &p : eval_positions) {
      bb_min = math::min(bb_min, p);
      bb_max = math::max(bb_max, p);
    }
  }
  const float diag = math::max(math::length(bb_max - bb_min), 1e-3f);

  /* Матрица мира и плоскости обзора для frustum-cull PBVH-узлов. */
  const float4x4 ob_to_world = ob->object_to_world();
  float frustum_planes[6][4];
  const int frustum_plane_len = build_frustum_planes(state.rv3d, frustum_planes);
  const bool use_frustum_cull = frustum_plane_len > 0;
  /* Ограничение на количество пересчётных нод за кадр (time budget, узлы). */
  const int recompute_budget_per_axis = 64;

  if (object_changed) {
    for (int axis = 0; axis < 3; axis++) {
      cached_segments_by_axis_[axis].clear();
    }
  }
  if (stroke_active || force_live) {
    /* Во время штриха пересчитываем все узлы, не опираясь на кэш. */
    for (int axis = 0; axis < 3; axis++) {
      cached_segments_by_axis_[axis].clear();
    }
  }

  BitVector<> dirty_lookup;
  if (has_dirty_nodes) {
    dirty_lookup.resize(pbvh->nodes_num(), false);
    dirty_nodes.to_bits(dirty_lookup);
  }
  else if (pbvh != nullptr) {
    CONTOUR_LOG("[CONTOUR_DEBUG] dirty_nodes empty; relying on force_live/stroke/cache clear\n");
  }

  bool pending_dirty_any_axis = false;

  for (int axis = 0; axis < 3; axis++) {
    const int axis_flag = (axis == 0) ? PAINT_SYMM_X : (axis == 1) ? PAINT_SYMM_Y : PAINT_SYMM_Z;
    if ((symmetry_flags & axis_flag) == 0) {
      continue;
    }

    PlaneData plane = build_plane_data(axis, 0.0f, ob->object_to_world());
    plane.quant_step = 1e-4f * diag;
    plane.min_seg_len = 1e-4f * diag;
    plane.min_loop_len = 1e-3f * diag;
    plane.plane_tolerance = 1e-5f * diag;
    plane.smooth_max_disp = 0.25f * plane.quant_step;

    Vector<ContourSegment> segments;
    blender::Set<uint64_t> segment_hashes;
    Map<int, Vector<ContourSegment>> &axis_cache = cached_segments_by_axis_[axis];
    /* Быстрый lookup кэша без мьютекса: снимок ключей в бит-вектор. */
    BitVector<> cache_lookup;
    int leaf_count = 0;
    std::atomic<int> recomputed_nodes(0), reused_nodes(0), skipped_nodes(0), frustum_culled_nodes(0);
    std::atomic<int> recompute_budget_left(recompute_budget_per_axis);
    std::atomic<bool> pending_dirty_nodes(false);
    std::mutex cache_mutex;
    std::mutex merge_mutex;

    if (pbvh != nullptr) {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*pbvh, memory);
      leaf_count = node_mask.size();

      Vector<int> node_indices(node_mask.size());
      node_mask.to_indices(node_indices.as_mutable_span());

      /* Заполняем битовую маску наличия кэша по индексам PBVH-нод. */
      if (!axis_cache.is_empty()) {
        cache_lookup.resize(pbvh->nodes_num(), false);
        axis_cache.foreach_item([&](const int key, const Vector<ContourSegment> & /*value*/) {
          if (key >= 0 && key < cache_lookup.size()) {
            cache_lookup[key].set(true);
          }
        });
      }

      switch (pbvh->type()) {
        case bke::pbvh::Type::Mesh: {
          Span<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
          threading::parallel_for(node_indices.index_range(), 256, [&](IndexRange range) {
            for (const int idx : node_indices.as_span().slice(range)) {
              const int n = idx;
              const bke::pbvh::MeshNode &node = nodes[n];
              if (!aabb_intersects_plane(node.bounds(), plane)) {
                skipped_nodes.fetch_add(1, std::memory_order_relaxed);
                continue;
              }
              if (use_frustum_cull) {
                const blender::Bounds<float3> world_bounds =
                    blender::bounds::transform_bounds<float, 4>(ob_to_world, node.bounds());
                const int isect = isect_aabb_planes_v3(
                    frustum_planes, frustum_plane_len, world_bounds.min, world_bounds.max);
                if (isect == ISECT_AABB_PLANE_BEHIND_ANY) {
                  frustum_culled_nodes.fetch_add(1, std::memory_order_relaxed);
                  continue;
                }
              }

              bool recompute = object_changed || has_dirty_nodes;
              if (!recompute) {
                /* Быстрая проверка по снимку кэша без мьютекса. */
                recompute = !cache_lookup.is_empty() ? !cache_lookup[n] : true;
              }
              else if (has_dirty_nodes) {
                recompute = dirty_lookup[n];
                if (!recompute && !object_changed) {
                  recompute = !cache_lookup.is_empty() ? !cache_lookup[n] : true;
                }
              }

              if (recompute) {
                int prev_budget = recompute_budget_left.fetch_sub(1, std::memory_order_relaxed);
                if (prev_budget <= 0) {
                  pending_dirty_nodes.store(true, std::memory_order_relaxed);
                  continue;
                }

                Vector<ContourSegment> node_segments;
                blender::Set<uint64_t> local_hashes;
                process_pbvh_mesh(eval_positions, &mesh, node, plane, node_segments, local_hashes);

                {
                  std::scoped_lock lock(cache_mutex);
                  axis_cache.add_overwrite(n, node_segments);
                  /* Обновляем снимок кэша для будущих кадров. */
                  if (!cache_lookup.is_empty() && n < cache_lookup.size()) {
                    cache_lookup[n].set(true);
                  }
                }

                {
                  std::scoped_lock lock(merge_mutex);
                  for (const ContourSegment &seg : node_segments) {
                    const uint64_t h = segment_hash(seg);
                    if (segment_hashes.add(h)) {
                      segments.append(seg);
                    }
                  }
                }
                recomputed_nodes.fetch_add(1, std::memory_order_relaxed);
              }
              else {
                const Vector<ContourSegment> *cached = axis_cache.lookup_ptr(n);
                if (cached != nullptr) {
                  std::scoped_lock lock(merge_mutex);
                  for (const ContourSegment &seg : *cached) {
                    const uint64_t h = segment_hash(seg);
                    if (segment_hashes.add(h)) {
                      segments.append(seg);
                    }
                  }
                  reused_nodes.fetch_add(1, std::memory_order_relaxed);
                }
              }
            }
          });
          break;
        }
        case bke::pbvh::Type::BMesh: {
          Span<bke::pbvh::BMeshNode> nodes = pbvh->nodes<bke::pbvh::BMeshNode>();
          threading::parallel_for(node_indices.index_range(), 256, [&](IndexRange range) {
            for (const int idx : node_indices.as_span().slice(range)) {
              const int n = idx;
              const bke::pbvh::BMeshNode &node = nodes[n];
              if (!aabb_intersects_plane(node.bounds(), plane)) {
                skipped_nodes.fetch_add(1, std::memory_order_relaxed);
                continue;
              }
              if (use_frustum_cull) {
                const blender::Bounds<float3> world_bounds =
                    blender::bounds::transform_bounds<float, 4>(ob_to_world, node.bounds());
                const int isect = isect_aabb_planes_v3(
                    frustum_planes, frustum_plane_len, world_bounds.min, world_bounds.max);
                if (isect == ISECT_AABB_PLANE_BEHIND_ANY) {
                  frustum_culled_nodes.fetch_add(1, std::memory_order_relaxed);
                  continue;
                }
              }

              bool recompute = object_changed || has_dirty_nodes;
              if (!recompute) {
                recompute = !cache_lookup.is_empty() ? !cache_lookup[n] : true;
              }
              else if (has_dirty_nodes) {
                recompute = dirty_lookup[n];
                if (!recompute && !object_changed) {
                  recompute = !cache_lookup.is_empty() ? !cache_lookup[n] : true;
                }
              }

              if (recompute) {
                int prev_budget = recompute_budget_left.fetch_sub(1, std::memory_order_relaxed);
                if (prev_budget <= 0) {
                  pending_dirty_nodes.store(true, std::memory_order_relaxed);
                  continue;
                }

                Vector<ContourSegment> node_segments;
                blender::Set<uint64_t> local_hashes;
                process_pbvh_bmesh(node, plane, node_segments, local_hashes);

                {
                  std::scoped_lock lock(cache_mutex);
                  axis_cache.add_overwrite(n, node_segments);
                  if (!cache_lookup.is_empty() && n < cache_lookup.size()) {
                    cache_lookup[n].set(true);
                  }
                }

                {
                  std::scoped_lock lock(merge_mutex);
                  for (const ContourSegment &seg : node_segments) {
                    const uint64_t h = segment_hash(seg);
                    if (segment_hashes.add(h)) {
                      segments.append(seg);
                    }
                  }
                }
                recomputed_nodes.fetch_add(1, std::memory_order_relaxed);
              }
              else {
                const Vector<ContourSegment> *cached = axis_cache.lookup_ptr(n);
                if (cached != nullptr) {
                  std::scoped_lock lock(merge_mutex);
                  for (const ContourSegment &seg : *cached) {
                    const uint64_t h = segment_hash(seg);
                    if (segment_hashes.add(h)) {
                      segments.append(seg);
                    }
                  }
                  reused_nodes.fetch_add(1, std::memory_order_relaxed);
                }
              }
            }
          });
          break;
        }
        case bke::pbvh::Type::Grids: {
          /* Обработка subdivision/multires геометрии через SubdivCCG. */
          const SculptSession *ss = ob->runtime->sculpt_session;
          if (!ss || !ss->subdiv_ccg) {
            break;
          }
          
          const SubdivCCG *subdiv_ccg = ss->subdiv_ccg;
          const CCGKey key = BKE_subdiv_ccg_key_top_level(*subdiv_ccg);
          
          Span<bke::pbvh::GridsNode> nodes = pbvh->nodes<bke::pbvh::GridsNode>();
          threading::parallel_for(node_indices.index_range(), 1, [&](const IndexRange range) {
            for (const int idx : node_indices.as_span().slice(range)) {
              const int n = idx;
              const bke::pbvh::GridsNode &node = nodes[n];
              
              /* AABB и frustum culling. */
              const blender::Bounds<float3> bounds = node.bounds();
              if (!aabb_intersects_plane(bounds, plane)) {
                skipped_nodes.fetch_add(1, std::memory_order_relaxed);
                continue;
              }
              
              /* Простая проверка bounds без детального frustum culling для первой версии. */
              if (state.rv3d && (bounds.min.x > 1000.0f || bounds.max.x < -1000.0f ||
                                bounds.min.y > 1000.0f || bounds.max.y < -1000.0f ||
                                bounds.min.z > 1000.0f || bounds.max.z < -1000.0f)) {
                frustum_culled_nodes.fetch_add(1, std::memory_order_relaxed);
                continue;
              }
              
              /* Проверка кэша и dirty состояния. */
              bool recompute = object_changed;
              if (!recompute && !cache_lookup.is_empty() && n < cache_lookup.size()) {
                if (!cache_lookup.is_empty()) {
                  recompute = !cache_lookup.is_empty() ? !cache_lookup[n] : true;
                }
              }
              
              /* Дополнительная проверка dirty для subdivision coords. */
              if (!recompute && subdiv_ccg->dirty.coords) {
                recompute = true;
              }
              
              if (recompute) {
                int prev_budget = recompute_budget_left.fetch_sub(1, std::memory_order_relaxed);
                if (prev_budget <= 0) {
                  pending_dirty_nodes.store(true, std::memory_order_relaxed);
                  continue;
                }
                
                Vector<ContourSegment> node_segments;
                blender::Set<uint64_t> local_hashes;
                process_pbvh_grids(*subdiv_ccg, key, node, plane, node_segments, local_hashes);
                
                {
                  std::scoped_lock lock(cache_mutex);
                  axis_cache.add_overwrite(n, node_segments);
                  if (!cache_lookup.is_empty() && n < cache_lookup.size()) {
                    cache_lookup[n].set(true);
                  }
                }
                
                {
                  std::scoped_lock lock(merge_mutex);
                  for (const ContourSegment &seg : node_segments) {
                    const uint64_t h = segment_hash(seg);
                    if (segment_hashes.add(h)) {
                      segments.append(seg);
                    }
                  }
                  recomputed_nodes.fetch_add(1, std::memory_order_relaxed);
                }
              } else {
                /* Переиспользуем кэшированные сегменты. */
                std::scoped_lock lock(cache_mutex);
                if (axis_cache.contains(n)) {
                  std::scoped_lock merge_lock(merge_mutex);
                  for (const ContourSegment &seg : axis_cache.lookup(n)) {
                    const uint64_t h = segment_hash(seg);
                    if (segment_hashes.add(h)) {
                      segments.append(seg);
                    }
                  }
                  reused_nodes.fetch_add(1, std::memory_order_relaxed);
                }
              }
            }
          });
          break;
        }
      }
    }

    /* Fallback: полный меш без PBVH. */
    if (segments.is_empty()) {
      SymmetryPlane plane_cpu{plane.normal, plane.point, 0.0f, plane.axis};
      Vector<IntersectionSegment> legacy = compute_intersections_cpu(&mesh, plane_cpu);
      for (const IntersectionSegment &seg : legacy) {
        append_segment(segments, segment_hashes, seg.start, seg.end, plane);
      }
      CONTOUR_LOG("[CONTOUR_DEBUG] Axis %d fallback legacy segments=%d\n", axis, int(segments.size()));
    }
    const int recompute_count = recomputed_nodes.load(std::memory_order_relaxed);
    const int reuse_count = reused_nodes.load(std::memory_order_relaxed);
    const int skip_count = skipped_nodes.load(std::memory_order_relaxed);
    const int frustum_count = frustum_culled_nodes.load(std::memory_order_relaxed);
    const int budget_left_val = recompute_budget_left.load(std::memory_order_relaxed);
    const bool pending_axis = pending_dirty_nodes.load(std::memory_order_relaxed);

    CONTOUR_LOG("[CONTOUR_DEBUG] Axis %d: leaf=%d recompute=%d reuse=%d skip=%d frustum=%d segments=%d\n",
                axis,
                leaf_count,
                recompute_count,
                reuse_count,
                skip_count,
                frustum_count,
                int(segments.size()));

    Vector<ContourLoop> axis_contours;
    build_contours_from_segments(segments, plane, axis_contours);
    CONTOUR_LOG("[CONTOUR_DEBUG] Axis %d after build_contours: segments=%d contours=%d\n",
                axis,
                int(segments.size()),
                int(axis_contours.size()));

    /* Fallback: если сборка ломаных не дала результата, отрисуем сырые сегменты. */
    if (axis_contours.is_empty() && !segments.is_empty()) {
      axis_contours.reserve(segments.size());
      for (const ContourSegment &seg : segments) {
        ContourLoop loop;
        loop.is_closed = false;
        loop.length = seg.length;
        loop.points.append(seg.a);
        loop.points.append(seg.b);
        axis_contours.append(loop);
      }
      int fallback_points = 0;
      for (const ContourLoop &contour : axis_contours) {
        fallback_points += contour.points.size();
      }
      CONTOUR_LOG("[CONTOUR_DEBUG] Axis %d after raw-seg fallback: contours=%d points=%d\n",
                  axis,
                  int(axis_contours.size()),
                  fallback_points);
    }

    /* Прореживание ломаных в экранном пространстве. */
    if (state.rv3d && state.region) {
      const float4x4 viewproj = float4x4(state.rv3d->persmat) * ob_to_world;
      const int2 region_size(state.region->winx, state.region->winy);
      const float pixel_step = 1.5f; /* Консервативный порог в пикселях. */
      for (ContourLoop &contour : axis_contours) {
        decimate_contour_screen(contour, viewproj, region_size, pixel_step);
      }
    }

    /* Метрики по количеству сегментов и вершин после сборки и decimate. */
    int contour_points_total = 0;
    for (const ContourLoop &contour : axis_contours) {
      contour_points_total += contour.points.size();
    }
    CONTOUR_LOG("[CONTOUR_DEBUG] Axis %d metrics: segments=%d contours=%d points=%d budget_left=%d pending=%d\n",
                axis,
                int(segments.size()),
                int(axis_contours.size()),
                contour_points_total,
                budget_left_val,
                int(pending_axis));

    for (ContourLoop &contour : axis_contours) {
      if (contour.points.size() < 2) {
        continue;
      }
      for (int i = 0; i < contour.points.size() - 1; i++) {
        contour_lines_.append(contour.points[i],
                              contour.points[i + 1],
                              float4(line_color_, line_alpha_));
      }
      if (contour.is_closed && contour.points.size() >= 3) {
        contour_lines_.append(contour.points.last(),
                              contour.points.first(),
                              float4(line_color_, line_alpha_));
      }
    }

    cached_contours_.extend(axis_contours);

    /* Если не все dirty-ноды пересчитаны из-за бюджета, форсим пересчёт в следующем кадре. */
    if (pending_dirty_nodes) {
      pending_dirty_any_axis = true;
    }
  }

  contours_dirty_ = pending_dirty_any_axis ? true : false;
  prev_enable_contours_ = enable_contours_;
  prev_object_ = ob;
  prev_symmetry_flags_ = symmetry_flags;
}

void SymmetryContour::end_sync(PassSimple::Sub &pass)
{
  CONTOUR_LOG("[CONTOUR_DEBUG] end_sync() called\n");
  CONTOUR_LOG("[CONTOUR_DEBUG] contour_lines_ buffer status\n");
  
  if (!enable_contours_) {
    CONTOUR_LOG("[CONTOUR_DEBUG] Early return: contours disabled\n");
    contour_lines_.clear();
    return;
  }

  /* Защита: если по каким-то причинам шейдер не установлен, не пытаться биндить SSBO. */
  if (contour_shader_ == nullptr) {
    CONTOUR_LOG("[CONTOUR_DEBUG] Early return: contour shader missing\n");
    contour_lines_.clear();
    return;
  }

  /* Обеспечиваем, что под-проход использует нужный шейдер, даже если предыдущие init/sub
   * не установили его (например, при повторном использовании sub). */
  pass.shader_set(contour_shader_);
  
  // Check if buffer has any data before trying to render
  // Note: We can't access data_buf directly, so we'll try to render and catch any issues
  CONTOUR_LOG("[CONTOUR_DEBUG] Attempting to render contours\n");
  
  CONTOUR_LOG("[CONTOUR_DEBUG] Rendering contours\n");

  pass.push_constant("contour_width", line_thickness_);
  
  // Finalize line buffer and submit to pass
  // end_sync() will check if data_buf is empty internally
  contour_lines_.end_sync(pass);
  
  CONTOUR_LOG("[CONTOUR_DEBUG] Contours rendered successfully\n");
}

int symmetry_flags_from_mesh_symmetry(char mesh_symmetry)
{
  int flags = 0;
  if (mesh_symmetry & ME_SYMMETRY_X) {
    flags |= PAINT_SYMM_X;
  }
  if (mesh_symmetry & ME_SYMMETRY_Y) {
    flags |= PAINT_SYMM_Y;
  }
  if (mesh_symmetry & ME_SYMMETRY_Z) {
    flags |= PAINT_SYMM_Z;
  }
  return flags;
}

int symmetry_flags_from_curves_symmetry(char curves_symmetry)
{
  int flags = 0;
  if (curves_symmetry & CURVES_SYMMETRY_X) {
    flags |= PAINT_SYMM_X;
  }
  if (curves_symmetry & CURVES_SYMMETRY_Y) {
    flags |= PAINT_SYMM_Y;
  }
  if (curves_symmetry & CURVES_SYMMETRY_Z) {
    flags |= PAINT_SYMM_Z;
  }
  return flags;
}

SymmetryContourOverlay::SymmetryContourOverlay(SelectionType selection_type, const char *pass_name)
    : pass_(pass_name), contour_(selection_type)
{
}

void SymmetryContourOverlay::begin_sync(Resources &res, const State &state, const bool show)
{
  show_ = show;

  if (!show_) {
    contour_.set_enabled(false);
    pass_.init();
    sub_ = nullptr;
    return;
  }

  contour_.set_enabled(true);
  contour_.begin_sync(res, state);

  pass_.init();
  pass_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
  pass_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
  {
    auto &sub = pass_.sub("Contours");
    /* NOTE: Keep depth test coherent with the line pre-pass.
     * Using DEPTH_ALWAYS in Texture Paint can make the line resolve drop the contour due to
     * depth mismatch in the line-AA pipeline. Draw order in overlay_instance already ensures this
     * pass runs after mesh line overlays in Texture Paint, so DEPTH_LESS_EQUAL is sufficient. */
    sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA,
                  state.clipping_plane_count);
    sub.shader_set(res.shaders->extra_wire_contour.get());
    sub_ = &sub;
  }
}

void SymmetryContourOverlay::object_sync(const Object *ob,
                                         const int symmetry_flags,
                                         const State &state)
{
  if (!show_) {
    return;
  }
  if (symmetry_flags == 0) {
    return;
  }
  contour_.update_contours(ob, symmetry_flags, state);
}

void SymmetryContourOverlay::end_sync()
{
  if (!show_) {
    return;
  }
  if (sub_ != nullptr) {
    contour_.end_sync(*sub_);
  }
}

void SymmetryContourOverlay::draw_line(Framebuffer &framebuffer, Manager &manager, View &view)
{
  if (!show_) {
    return;
  }
  GPU_framebuffer_bind(framebuffer);
  manager.submit(pass_, view);
}

}  // namespace blender::draw::overlay
