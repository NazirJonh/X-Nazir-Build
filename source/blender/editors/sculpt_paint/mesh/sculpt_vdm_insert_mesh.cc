/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Vector Displacement Map (VDM) "insert mesh" tool.
 *
 * A VDM Draw stroke normally deforms the active mesh. In insert-mesh mode it instead behaves
 * like a stamp: the brush state at the first dab of the stroke is captured (see #VDMStampData)
 * and, once the stroke finishes, a brand new watertight volume is generated from the texture.
 *
 * The generated volume is a closed solid made of three parts:
 * - a displaced top surface sampled from the VDM texture, conformed to the target mesh's surface
 *   underneath the brush footprint (see #raycast_stamp_footprint_point) rather than sitting on a
 *   flat brush plane,
 * - a bottom that follows the same conformed base, offset along each vertex's locally conformed
 *   surface normal so it (and the skirt connecting it to the top) keeps hugging curved geometry
 *   near the footprint's edges,
 * - a side wall (skirt) connecting their borders.
 *
 * This makes the result directly usable for boolean/join operations against the main mesh,
 * which is the whole point: a VDM brush becomes a lightweight, instantly placed and scaled
 * source of separate geometry the artist can refine before merging.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_storage.hh"
#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "BLI_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#ifdef WITH_TBB
#  include <tbb/task_arena.h>
#endif

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"

#include "../paint_intern.hh"
#include "sculpt_face_set.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

/**
 * TBB nested parallelism is unavailable in the sculpt stroke cleanup context
 * (#tbb::this_task_arena::max_concurrency() == 1, including on fresh #std::thread workers).
 * Use explicit OS threads for the hot loops below instead of #threading::parallel_for.
 */
static int vdm_worker_count(const int64_t work_size, const int64_t grain_size)
{
  const int max_workers = std::max(1, BLI_task_scheduler_num_threads());
  if (work_size <= grain_size) {
    return 1;
  }
  const int64_t max_tasks_by_grain = (work_size + grain_size - 1) / grain_size;
  return int(std::min<int64_t>(max_workers, max_tasks_by_grain));
}

template<typename Function>
static void vdm_parallel_for(const IndexRange range,
                             const int64_t grain_size,
                             const Function &fn)
{
  const int num_tasks = vdm_worker_count(range.size(), grain_size);
  if (num_tasks <= 1) {
    fn(range, 0);
    return;
  }

  const int64_t chunk = (range.size() + num_tasks - 1) / num_tasks;
  Vector<IndexRange> sub_ranges;
  Vector<int> task_indices;
  sub_ranges.reserve(num_tasks);
  task_indices.reserve(num_tasks);
  for (int task = 0; task < num_tasks; task++) {
    const int64_t begin = range.start() + int64_t(task) * chunk;
    if (begin >= range.one_after_last()) {
      break;
    }
    const int64_t end = std::min(begin + chunk, range.one_after_last());
    sub_ranges.append(IndexRange::from_begin_end(begin, end));
    task_indices.append(task);
  }

  Vector<std::thread> threads;
  threads.reserve(sub_ranges.size());
  for (const int i : sub_ranges.index_range()) {
    const IndexRange sub_range = sub_ranges[i];
    const int task_index = task_indices[i];
    threads.append(std::thread([&, sub_range, task_index]() { fn(sub_range, task_index); }));
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
}

template<typename Value, typename Body, typename Merge>
static Value vdm_parallel_reduce(const IndexRange range,
                                 const int64_t grain_size,
                                 const Value &identity,
                                 const Body &body,
                                 const Merge &merge)
{
  const int num_tasks = vdm_worker_count(range.size(), grain_size);
  if (num_tasks <= 1) {
    return body(range, identity);
  }

  const int64_t chunk = (range.size() + num_tasks - 1) / num_tasks;
  Vector<IndexRange> sub_ranges;
  sub_ranges.reserve(num_tasks);
  for (int task = 0; task < num_tasks; task++) {
    const int64_t begin = range.start() + int64_t(task) * chunk;
    if (begin >= range.one_after_last()) {
      break;
    }
    const int64_t end = std::min(begin + chunk, range.one_after_last());
    sub_ranges.append(IndexRange::from_begin_end(begin, end));
  }

  Vector<Value> partials(sub_ranges.size(), identity);
  Vector<std::thread> threads;
  threads.reserve(sub_ranges.size());
  for (const int i : sub_ranges.index_range()) {
    const IndexRange sub_range = sub_ranges[i];
    threads.append(std::thread([&, sub_range, i]() { partials[i] = body(sub_range, identity); }));
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  Value result = identity;
  for (const Value &part : partials) {
    result = merge(result, part);
  }
  return result;
}

/* -------------------------------------------------------------------- */
/** \name Performance Logging
 *
 * Pass a non-null pointer to #VDMPerfLog into the key functions to collect per-phase timings.
 * Null disables all measurement with zero overhead (branch is always predictable).
 *
 * Reading the output:
 *   - "stamp_displacement (sum)" counts wall-ns across ALL threads combined.
 *     Compare it to "Pass1 parallel_for (wall) * thread_count" to gauge parallelism.
 *   - If "Pass1 (wall)" dominates everything else → texture sampling is the bottleneck;
 *     lowering resolution or caching the texture would help.
 *   - If "face build" is large relative to Pass1 → the skirt-quad loop is the target.
 *   - "active quads %" drives how much geometry both pass1 and face-build actually emit.
 * \{ */

struct VDMPerfLog {
  using Clock = std::chrono::steady_clock;

  static int64_t since(Clock::time_point t0)
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
  }

  /* Sequential timings (nanoseconds). Accumulated across all stamps. */
  int64_t resolution_ns = 0;        /* resolution_from_mesh_density() */
  int64_t build_mesh_ns = 0;        /* build_stamps_mesh() total */
  int64_t pass1_wall_ns = 0;        /* threading::parallel_for wall time */
  int64_t surface_conform_ns = 0;   /* Pass 1.5 surface raycast + bilinear height blend */
  int64_t baseline_ns = 0;          /* border baseline accumulation loop */
  int64_t active_compute_ns = 0;    /* disp subtract + active_lens per vertex */
  int64_t quad_mark_ns = 0;         /* quad_active[][] marking */
  int64_t vert_alloc_ns = 0;        /* vert_needed + top/bottom vertex push */
  int64_t face_build_ns = 0;        /* top cap + bottom cap + skirt quads */
  int64_t join_ns = 0;              /* mesh append + BKE_mesh_nomain_to_mesh (wall) */
  int64_t join_append_ns = 0;       /* bke::mesh_append */
  int64_t join_nomain_to_mesh_ns = 0; /* BKE_mesh_nomain_to_mesh */
  int64_t total_exec_ns = 0;        /* insert_mesh_exec() total */

  /* Thread-safe: accumulated from parallel_for workers. */
  std::atomic<int64_t> stamp_disp_ns{0}; /* stamp_displacement() summed over all threads */
  std::atomic<int> pass1_max_thread_id{0}; /* highest thread_id seen — proxy for thread count */
  std::atomic<uint64_t> pass1_os_threads_mask{0}; /* actual OS/TBB threads observed in Pass1 */
  std::atomic<uint64_t> res_os_threads_mask{0};   /* actual OS/TBB threads observed in PBVH reduce */
  std::atomic<int> pass1_manual_threads{0};
  std::atomic<int> res_manual_threads{0};

  /* Diagnostics for the PBVH density scan: how much work the foreach_index pass really does.
   * `faces_scanned` counts every face of every intersecting node (including faces whose edges
   * fall outside the radius). `edges_tested` counts the per-corner distance checks. Comparing
   * them to `edges_in_radius` shows how much of the scan is wasted and what sample size an
   * early-exit estimate would actually need. */
  std::atomic<int64_t> res_faces_scanned{0};
  std::atomic<int64_t> res_edges_tested{0};

  /* Sub-timings for resolution_from_mesh_density (nanoseconds). */
  int64_t res_search_nodes_ns = 0;  /* bke::pbvh::search_nodes PBVH traversal */
  int64_t res_pbvh_nodes_ns = 0;    /* foreach_index + edge_density_from_faces per node */
  int64_t res_fallback_ns = 0;      /* edge_density_from_all_mesh_edges full-mesh fallback */
  int res_pbvh_nodes_count = 0;     /* PBVH nodes intersecting the brush footprint */
  int64_t res_edges_in_radius = 0;  /* edges whose midpoint is inside the brush sphere (counted) */
  bool res_used_pbvh = false;       /* true = PBVH path succeeded */
  bool res_used_fallback = false;   /* true = fell back to full O(all_edges) scan */

  /* Counters. Accumulated across all stamps. */
  int resolution = 0;
  int stamps_num = 0;
  int scheduler_threads = 0;
  int arena_max_concurrency = 0;
  int arena_thread_index = 0;
  int probe_threads = 0;
  int probe_explicit_arena_threads = 0;
  int probe_os_thread_threads = 0;
  int probe_manual_threads = 0;
  int active_quads = 0;
  int total_quads = 0;
  int verts_emitted = 0;
  int faces_emitted = 0;
  int join_active_verts = 0;
  int join_active_faces = 0;
  int join_stamp_verts = 0;
  int join_stamp_faces = 0;

  static void record_current_thread(std::atomic<uint64_t> &mask)
  {
    const uint64_t bit = uint64_t(1) << (std::hash<std::thread::id>{}(std::this_thread::get_id()) & 63);
    mask.fetch_or(bit, std::memory_order_relaxed);
  }

  static void record_thread_once(std::atomic<int> &count)
  {
    thread_local bool registered = false;
    if (!registered) {
      registered = true;
      count.fetch_add(1, std::memory_order_relaxed);
    }
  }

  static int approx_thread_count(const std::atomic<uint64_t> &mask)
  {
    uint64_t bits = mask.load(std::memory_order_relaxed);
    int count = 0;
    while (bits != 0) {
      bits &= bits - 1;
      count++;
    }
    return count;
  }

  /**
   * Capture the active TBB arena state and run lightweight #parallel_for probes.
   * The default-arena probe shows whether the operator context itself can spawn workers.
   * The explicit-arena probe checks whether escaping to a dedicated #tbb::task_arena restores
   * parallelism (as in #threading::detail::memory_bandwidth_bound_task_impl).
   */
  void run_threading_diagnostics()
  {
#ifdef WITH_TBB
    arena_max_concurrency = tbb::this_task_arena::max_concurrency();
    arena_thread_index = tbb::this_task_arena::current_thread_index();
#else
    arena_max_concurrency = 1;
    arena_thread_index = 0;
#endif

    std::atomic<uint64_t> probe_mask{0};
    threading::parallel_for(IndexRange(65536), 1024, [&](const IndexRange range) {
      record_current_thread(probe_mask);
      for ([[maybe_unused]] const int64_t i : range) {
        /* Touch the loop variable so the compiler cannot delete the iteration. */
      }
    });
    probe_threads = approx_thread_count(probe_mask);

#ifdef WITH_TBB
    std::atomic<uint64_t> explicit_mask{0};
    tbb::task_arena arena(scheduler_threads);
    arena.execute([&] {
      threading::parallel_for(IndexRange(65536), 1024, [&](const IndexRange range) {
        record_current_thread(explicit_mask);
        for ([[maybe_unused]] const int64_t i : range) {
        }
      });
    });
    probe_explicit_arena_threads = approx_thread_count(explicit_mask);
#else
    probe_explicit_arena_threads = probe_threads;
#endif

    std::atomic<uint64_t> os_thread_mask{0};
    std::thread([&] {
      threading::parallel_for(IndexRange(65536), 1024, [&](const IndexRange range) {
        record_current_thread(os_thread_mask);
        for ([[maybe_unused]] const int64_t i : range) {
        }
      });
    }).join();
    probe_os_thread_threads = approx_thread_count(os_thread_mask);

    std::atomic<int> manual_count{0};
    vdm_parallel_for(IndexRange(65536), 1024, [&](const IndexRange range, const int /*task_index*/) {
      record_thread_once(manual_count);
      for ([[maybe_unused]] const int64_t i : range) {
      }
    });
    probe_manual_threads = manual_count.load(std::memory_order_relaxed);
  }

  void print() const
  {
    const auto ms = [](int64_t ns) -> double { return double(ns) * 1e-6; };
    const int64_t disp_sum = stamp_disp_ns.load(std::memory_order_relaxed);
    const int max_tid = pass1_max_thread_id.load(std::memory_order_relaxed);
    /* Estimated thread count: highest observed thread_id + 1.
     * When max_tid == 0 the parallel_for ran on a single thread (no worker ever got id > 0). */
    const int est_threads = max_tid + 1;
    const int pass1_manual = pass1_manual_threads.load(std::memory_order_relaxed);
    const int res_manual = res_manual_threads.load(std::memory_order_relaxed);
    const double parallel_efficiency =
        (pass1_wall_ns > 0 && est_threads > 1) ?
            100.0 * (double(disp_sum) / double(est_threads)) / double(pass1_wall_ns) :
            0.0;

    printf("\n[VDM PERF] resolution=%d  stamps=%d  scheduler_threads=%d  "
           "arena_max_concurrency=%d  arena_thread_index=%d\n",
           resolution,
           stamps_num,
           scheduler_threads,
           arena_max_concurrency,
           arena_thread_index);
    printf("  threading probe (default arena)  : actual threads ~= %d%s\n",
           probe_threads,
           probe_threads <= 1 ? " — SINGLE-THREADED!" : "");
    printf("  threading probe (explicit arena) : actual threads ~= %d%s\n",
           probe_explicit_arena_threads,
           probe_explicit_arena_threads <= 1 ? " — SINGLE-THREADED!" : "");
    printf("  threading probe (OS thread)      : actual threads ~= %d%s\n",
           probe_os_thread_threads,
           probe_os_thread_threads <= 1 ? " — SINGLE-THREADED!" : "");
    printf("  threading probe (manual threads) : actual threads ~= %d%s\n",
           probe_manual_threads,
           probe_manual_threads <= 1 ? " — SINGLE-THREADED!" : "");

    /* --- resolution_from_mesh_density breakdown --- */
    printf("  resolution_from_mesh_density : %8.2f ms\n", ms(resolution_ns));
    if (res_used_pbvh) {
      printf("    PBVH path (succeeded)\n");
      printf("      search_nodes             : %8.2f ms  (%d nodes hit)\n",
             ms(res_search_nodes_ns),
             res_pbvh_nodes_count);
      printf("      foreach_index+faces      : %8.2f ms  (manual threads ~= %d)\n",
             ms(res_pbvh_nodes_ns),
             res_manual);
      {
        const int64_t faces_scanned = res_faces_scanned.load(std::memory_order_relaxed);
        const int64_t edges_tested = res_edges_tested.load(std::memory_order_relaxed);
        const double in_radius_pct =
            edges_tested ? 100.0 * double(res_edges_in_radius) / double(edges_tested) : 0.0;
        printf("        faces scanned          : %lld  (whole-node faces, incl. outside radius)\n",
               (long long)faces_scanned);
        printf("        edges tested           : %lld  (per-corner distance checks)\n",
               (long long)edges_tested);
        printf("        edges in radius        : %lld  (%.1f%% of tested; ~2x unique, shared edges)\n",
               (long long)res_edges_in_radius,
               in_radius_pct);
      }
    }
    if (res_used_fallback) {
      printf("    fallback O(all_edges)      : %8.2f ms  <- full mesh scan!\n",
             ms(res_fallback_ns));
    }
    if (!res_used_pbvh && !res_used_fallback) {
      printf("    (no PBVH, no fallback — mesh has no edges?)\n");
    }

    /* --- build_stamps_mesh --- */
    printf("  build_stamps_mesh (total)    : %8.2f ms\n", ms(build_mesh_ns));
    printf("    Pass1 parallel_for (wall)  : %8.2f ms  (texture thread_ids: %d, manual threads ~= %d%s)\n",
           ms(pass1_wall_ns),
           est_threads,
           pass1_manual,
           pass1_manual <= 1 ? " — SINGLE-THREADED!" : "");
    printf("    stamp_displacement (sum)   : %8.2f ms  (all threads combined)\n", ms(disp_sum));
    if (est_threads > 1) {
      printf("    parallel efficiency        :  %.1f%%\n", parallel_efficiency);
    }
    printf("    surface conform (raycast)  : %8.2f ms\n", ms(surface_conform_ns));
    printf("    baseline loop              : %8.2f ms\n", ms(baseline_ns));
    printf("    active_lens compute        : %8.2f ms\n", ms(active_compute_ns));
    printf("    quad_active mark           : %8.2f ms\n", ms(quad_mark_ns));
    printf("    vert alloc                 : %8.2f ms\n", ms(vert_alloc_ns));
    printf("    face build (cap+skirt)     : %8.2f ms\n", ms(face_build_ns));
    printf("    active quads               : %d / %d  (%.1f%%)\n",
           active_quads,
           total_quads,
           total_quads ? 100.0 * active_quads / total_quads : 0.0);
    printf("    verts emitted              : %d\n", verts_emitted);
    printf("    faces emitted              : %d\n", faces_emitted);

    if (join_ns > 0) {
      printf("  join / mesh merge (total)    : %8.2f ms\n", ms(join_ns));
      printf("    mesh append (fast)         : %8.2f ms  "
             "(active %d verts / %d faces + stamp %d / %d)\n",
             ms(join_append_ns),
             join_active_verts,
             join_active_faces,
             join_stamp_verts,
             join_stamp_faces);
      printf("    BKE_mesh_nomain_to_mesh    : %8.2f ms\n", ms(join_nomain_to_mesh_ns));
    }
    printf("  TOTAL exec                   : %8.2f ms\n", ms(total_exec_ns));
    printf("\n");
    fflush(stdout);
  }
};

/** \} */

static bool insert_mesh_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (ob != nullptr && ob->mode == OB_MODE_SCULPT) {
    if (BKE_sculpt_multires_active(CTX_data_scene(C), ob)) {
      CTX_wm_operator_poll_msg_set(C, "Insert Mesh is not supported on a multires object");
      return false;
    }
    return ED_operator_object_active_editable_mesh(C);
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name VDM Sampling
 *
 * Both functions reproduce the math used while sculpting (`sculpt_apply_texture()` and
 * `calc_vertex_displacement()`) so the generated shape matches what the VDM Draw brush would
 * have carved into the active mesh. The brush state is read from the captured #VDMStampData
 * instead of the live #StrokeCache.
 * \{ */

/**
 * Object-space position of a point on the flat brush plane.
 *
 * \param uv: normalized brush-plane coordinates in [-1, 1]. The matrix already embeds the brush
 * location, orientation and radius, so the resulting point spans the full brush footprint.
 */
static float3 stamp_plane_point(const VDMStampData &stamp, const float2 &uv)
{
  float3 co(uv.x, uv.y, 0.0f);
  mul_m4_v3(stamp.brush_local_mat_inv.ptr(), co);
  return co;
}

/**
 * Stamp displacement for a plane point, in object space.
 *
 * Mirrors `sculpt_apply_texture()` (the `MTEX_MAP_MODE_AREA` branch) for the texture lookup. The
 * sampled texel is then turned into an object-space vector in one of two ways:
 * - Vector displacement (VDM): the RGB is a displacement vector, matching `calc_vertex_displacement()`.
 * - Plain alpha texture: the scalar value is a height along the brush normal (local +Z), matching
 *   the regular Draw brush (see #do_draw_brush).
 *
 * In both cases the final vector is transformed by `brush_local_mat_inv`, which already embeds the
 * brush radius and orientation, and then mapped back through the active symmetry pass.
 *
 * \a mtex is the brush's sculpt-mode texture, fetched once by the caller. Its contents are the same
 * for every grid point of a stamp, so re-fetching it per call would be wasted work; the caller is
 * also responsible for the null-texture check and skips the stamp entirely when none is bound.
 * \a tex_pool is the image pool the lookup draws thread-local buffers from, and \a thread_id selects
 * the per-thread slot within it — that is what makes the lookup safe to run inside the parallel
 * sampling loop (the same mechanism the live VDM Draw brush relies on, see #do_draw_brush).
 *
 * \param r_has_data: false when the raw sampled texel is (near-)black -- the convention VDM
 * textures use for "nothing painted here" -- and true otherwise. The caller uses this to keep
 * unpainted texels from contributing geometry: unlike the mid-grey neutral of e.g. tangent-space
 * normal maps, black does not decode to a zero displacement vector once #Brush::texture_sample_bias
 * is added below, so leaving those texels to the generic activity threshold risks spurious spikes
 * of geometry wherever the footprint extends past the painted area (see the caller).
 */
static float3 stamp_displacement(const VDMStampData &stamp,
                                 const Brush &brush,
                                 const MTex *mtex,
                                 ImagePool *tex_pool,
                                 const float3 &plane_co,
                                 const int thread_id,
                                 const bool use_vector_displacement,
                                 const float3 &vdm_axis_scale,
                                 bool &r_has_data)
{
  /* Sampling point: `sculpt_apply_texture()` starts from `(brush_point - plane_offset)`. */
  float3 point = plane_co - stamp.plane_offset;
  if (stamp.radial_symmetry_pass) {
    mul_m4_v3(stamp.symm_rot_mat_inv.ptr(), point);
  }
  float3 symm_point = symmetry_flip(point, stamp.mirror_symmetry_pass);
  mul_m4_v3(stamp.brush_local_mat.ptr(), symm_point);

  const float x = symm_point.x * mtex->size[0] + mtex->ofs[0];
  const float y = symm_point.y * mtex->size[1] + mtex->ofs[1];

  float value;
  float4 rgba;
  paint_get_tex_pixel(mtex, x, y, tex_pool, thread_id, &value, rgba);

  float3 disp;
  if (use_vector_displacement) {
    /* No alpha channel marks painted vs. unpainted area on these textures, so this has to work
     * from color alone. The border between painted content and the black background is rarely a
     * hard edge -- texture filtering and lossy compression fade it toward black over a few texels
     * -- so a near-zero epsilon only catches the interior of the padding and leaves that fringe
     * classified as "real" data, which is what fragments the mesh right at the content boundary.
     * This is deliberately generous enough to absorb that fade; the padding flood fill in the
     * caller only pulls in no-data texels that chain back to the grid border, so being generous
     * here costs a few texels of genuine content right at the edge, not interior detail. */
    constexpr float black_epsilon = 0.05f;
    r_has_data = !(rgba.x < black_epsilon && rgba.y < black_epsilon && rgba.z < black_epsilon);

    /* Color -> object-space displacement, matching `calc_vertex_displacement()`. */
    add_v3_fl(rgba, brush.texture_sample_bias);
    disp = float3(rgba);
    mul_v3_fl(disp, stamp.bstrength);
    if (stamp.bstrength < 0.0f) {
      disp.x *= -1.0f;
      disp.y *= -1.0f;
    }
    disp *= vdm_axis_scale;
  }
  else {
    r_has_data = true;

    /* Plain alpha texture: height field along the brush normal (local +Z). The brush-local matrix
     * already embeds the brush radius, so the local height only needs the sampled value scaled by
     * the brush strength. Negative strength simply carves inward. Matches `sculpt_apply_texture()`'s
     * `*r_value -= texture_sample_bias`. */
    value -= brush.texture_sample_bias;
    disp = float3(0.0f, 0.0f, value * stamp.bstrength);
  }

  mul_mat3_m4_v3(stamp.brush_local_mat_inv.ptr(), disp);
  if (stamp.radial_symmetry_pass) {
    mul_m4_v3(stamp.symm_rot_mat.ptr(), disp);
  }
  return symmetry_flip(disp, stamp.mirror_symmetry_pass);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Volume Construction
 * \{ */

struct EdgeDensity {
  float total_length = 0.0f;
  int count = 0;
};

static int resolution_from_edge_density(const EdgeDensity &density, const float radius)
{
  if (density.count == 0 || density.total_length == 0.0f) {
    return 32;
  }

  const float avg_edge_len = density.total_length / float(density.count);
  const int resolution = int(2.0f * radius / avg_edge_len);
  return math::clamp(resolution, 2, 256);
}

static bool bounds_intersects_sphere(const Bounds<float3> &bounds,
                                     const float3 &center,
                                     const float radius_sq)
{
  float dist_sq = 0.0f;
  for (const int axis : IndexRange(3)) {
    if (center[axis] < bounds.min[axis]) {
      dist_sq += math::square(bounds.min[axis] - center[axis]);
    }
    else if (center[axis] > bounds.max[axis]) {
      dist_sq += math::square(center[axis] - bounds.max[axis]);
    }
  }
  return dist_sq <= radius_sq;
}

static EdgeDensity edge_density_from_faces(const Mesh &mesh,
                                           const Span<int> face_indices,
                                           const float3 &center,
                                           const float radius_sq,
                                           int64_t *r_edges_tested = nullptr)
{
  const Span<float3> positions = mesh.vert_positions();
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();

  EdgeDensity density;
  for (const int face : face_indices) {
    const Span<int> face_verts = corner_verts.slice(faces[face]);
    if (r_edges_tested) {
      *r_edges_tested += face_verts.size();
    }
    for (const int i : face_verts.index_range()) {
      const int vert_a = face_verts[i];
      const int vert_b = face_verts[(i + 1) % face_verts.size()];
      const float3 mid = (positions[vert_a] + positions[vert_b]) * 0.5f;
      const float3 to_mid = mid - center;
      if (math::dot(to_mid, to_mid) <= radius_sq) {
        density.total_length += math::length(positions[vert_b] - positions[vert_a]);
        density.count++;
      }
    }
  }
  return density;
}

static EdgeDensity edge_density_from_all_mesh_edges(const Mesh &mesh,
                                                    const float3 &center,
                                                    const float radius_sq)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> edges = mesh.edges();

  return vdm_parallel_reduce(
      edges.index_range(),
      4096,
      EdgeDensity(),
      [&](const IndexRange range, EdgeDensity local) {
        for (const int i : range) {
          const int2 edge = edges[i];
          const float3 mid = (positions[edge[0]] + positions[edge[1]]) * 0.5f;
          const float3 to_mid = mid - center;
          if (math::dot(to_mid, to_mid) <= radius_sq) {
            local.total_length += math::length(positions[edge[1]] - positions[edge[0]]);
            local.count++;
          }
        }
        return local;
      },
      [](const EdgeDensity &a, const EdgeDensity &b) {
        return EdgeDensity{a.total_length + b.total_length, a.count + b.count};
      });
}

/**
 * Estimate a stamp grid resolution that matches the polygon density of \a mesh at the brush
 * footprint.
 *
 * When a mesh PBVH is available, scan only leaf nodes whose bounds intersect the stamp footprint.
 * This avoids a full O(edges) pass over dense sculpt meshes for every insert. Falls back to the
 * full edge scan when PBVH data is unavailable or finds no local faces.
 */
static int resolution_from_mesh_density(const Mesh &mesh,
                                        const bke::pbvh::Tree *pbvh,
                                        const float3 &center,
                                        const float radius,
                                        VDMPerfLog *perf = nullptr)
{
  const float radius_sq = radius * radius;

  if (pbvh && pbvh->type() == bke::pbvh::Type::Mesh) {
    IndexMaskMemory memory;

    VDMPerfLog::Clock::time_point t_search;
    if (perf) {
      t_search = VDMPerfLog::Clock::now();
    }

    const IndexMask node_mask = bke::pbvh::search_nodes(
        *pbvh, memory, [&](const bke::pbvh::Node &node) {
          return bounds_intersects_sphere(node.bounds(), center, radius_sq);
        });

    if (perf) {
      perf->res_search_nodes_ns = VDMPerfLog::since(t_search);
      perf->res_pbvh_nodes_count = int(node_mask.size());
    }

    if (!node_mask.is_empty()) {
      const Span<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();

      VDMPerfLog::Clock::time_point t_nodes;
      if (perf) {
        t_nodes = VDMPerfLog::Clock::now();
      }

      /* The resolution only depends on the *average* edge length at the footprint, a statistically
       * stable quantity: on a locally near-uniform sculpt mesh a couple thousand sampled edges
       * reproduce the full-scan average to well within the integer resolution step. Scanning every
       * footprint edge (often ~1M on dense meshes) just to derive one average is wasteful, so
       * accumulate node-by-node and stop once enough in-radius edges are gathered.
       *
       * Each `positions[vert]` is a cache-cold random read into a multi-million-vertex array, so the
       * scan is memory-latency bound (~500 ns/edge single-threaded). Keep the sample small: the time
       * scales linearly with it, and spawning the manual worker threads to hide the latency would
       * cost more (1-2 ms just to spawn) than the whole sampled scan. */
      constexpr int sample_target = 4096;
      EdgeDensity density;
      int64_t faces_scanned = 0;
      int64_t edges_tested = 0;
      node_mask.foreach_index([&](const int node_i) {
        if (density.count >= sample_target) {
          return;
        }
        const Span<int> node_faces = nodes[node_i].faces();
        faces_scanned += node_faces.size();
        const EdgeDensity nd = edge_density_from_faces(
            mesh, node_faces, center, radius_sq, perf ? &edges_tested : nullptr);
        density.total_length += nd.total_length;
        density.count += nd.count;
      });

      if (perf) {
        perf->res_pbvh_nodes_ns = VDMPerfLog::since(t_nodes);
        perf->res_edges_in_radius = density.count;
        perf->res_faces_scanned.fetch_add(faces_scanned, std::memory_order_relaxed);
        perf->res_edges_tested.fetch_add(edges_tested, std::memory_order_relaxed);
      }

      if (density.count != 0 && density.total_length != 0.0f) {
        if (perf) {
          perf->res_used_pbvh = true;
        }
        return resolution_from_edge_density(density, radius);
      }
    }
  }

  VDMPerfLog::Clock::time_point t_fallback;
  if (perf) {
    perf->res_used_fallback = true;
    t_fallback = VDMPerfLog::Clock::now();
  }

  const EdgeDensity density = edge_density_from_all_mesh_edges(mesh, center, radius_sq);

  if (perf) {
    perf->res_fallback_ns = VDMPerfLog::since(t_fallback);
  }

  return resolution_from_edge_density(density, radius);
}

/**
 * Raycast a single stamp-footprint point against the sculpted mesh to find where its actual
 * surface lies there. Used to conform the generated volume's base to curved geometry instead of
 * always sitting on the brush's flat tangent plane (see the module docstring).
 *
 * Casts from outside the footprint toward the brush plane along the brush normal. Returns false
 * when the ray misses the mesh (e.g. the footprint extends past its silhouette), or when the
 * closest hit's face isn't reasonably outward-facing relative to the brush (e.g. the ray slipped
 * sideways into the wall of an unrelated recess or an adjacent stamp instead of the surface
 * directly beneath this footprint point). Either way the caller keeps the flat plane height for
 * that sample rather than risk conforming to unrelated geometry.
 *
 * \note Must be called sequentially, never concurrently on the same \a pbvh from multiple
 * threads: #bke::pbvh::raycast() writes into `Node::tmin_` for traversal pruning, which races
 * under concurrent writers.
 */
static bool raycast_stamp_footprint_point(bke::pbvh::Tree &pbvh,
                                          const Mesh &mesh,
                                          const float3 &plane_point,
                                          const float3 &brush_normal,
                                          const float radius,
                                          float3 &r_hit_position,
                                          float3 &r_hit_normal)
{
  const float3 ray_normal = -brush_normal;
  const float3 ray_start = plane_point - ray_normal * (radius * 4.0f);

  const Span<float3> vert_positions = mesh.vert_positions();
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> corner_tris = mesh.corner_tris();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<bool> hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);

  IsectRayPrecalc isect_precalc;
  isect_ray_tri_watertight_v3_precalc(&isect_precalc, ray_normal);

  float depth = radius * 8.0f;
  bool hit = false;
  int active_vert = -1;
  int active_face = -1;
  float3 face_normal;

  bke::pbvh::raycast(
      pbvh,
      [&](bke::pbvh::Node &node, float *tmin) {
        if (BKE_pbvh_node_get_tmin(&node) >= *tmin) {
          return;
        }
        if (bke::pbvh::node_raycast_mesh(static_cast<bke::pbvh::MeshNode &>(node),
                                         {},
                                         vert_positions,
                                         faces,
                                         corner_verts,
                                         corner_tris,
                                         hide_poly,
                                         ray_start,
                                         ray_normal,
                                         &isect_precalc,
                                         &depth,
                                         active_vert,
                                         active_face,
                                         face_normal))
        {
          hit = true;
          *tmin = depth;
        }
      },
      ray_start,
      ray_normal,
      false);

  if (!hit) {
    return false;
  }

  /* Reject hits that don't face roughly the same way as the brush: those belong to a wall
   * transverse to the footprint (an unrelated recess, an overhang, geometry seen edge-on near
   * the object's silhouette) rather than the surface patch this footprint point should conform
   * to. A single such outlier would otherwise corrupt a whole rectangular region of the bilinear
   * height blend in the caller. */
  constexpr float min_normal_alignment = 0.3f;
  if (math::dot(face_normal, brush_normal) < min_normal_alignment) {
    return false;
  }

  r_hit_position = ray_start + ray_normal * depth;
  r_hit_normal = face_normal;
  return true;
}

/**
 * Append one watertight VDM stamp volume to the mesh buffers.
 *
 * Only grid quads that carry meaningful VDM displacement are generated — flat border padding
 * common in VDM textures is silently skipped. The resulting mesh therefore conforms to the
 * actual shape encoded in the texture rather than always being a rectangular block.
 *
 * A quad is "active" when at least one of its four corner vertices has displacement whose
 * magnitude exceeds 1 % of the grid cell size. Watertightness is preserved by emitting skirt
 * faces on every edge that borders an inactive quad (or the grid boundary). When the VDM
 * contains a recess, the bottom cap is sunk further below the deepest displaced vertex so the
 * volume fully encloses concave geometry without self-intersection.
 *
 * Face winding is emitted outward directly so no BMesh normal-recalculation pass is needed.
 */
static void add_stamp_to_mesh_buffers(Vector<float3> &positions,
                                      Vector<int> &face_offsets,
                                      Vector<int> &corner_verts,
                                      const VDMStampData &stamp,
                                      const Brush &brush,
                                      SculptSession &ss,
                                      bke::pbvh::Tree *pbvh,
                                      const Mesh &active_mesh,
                                      const int resolution,
                                      const bool use_vector_displacement,
                                      const float3 &vdm_axis_scale,
                                      VDMPerfLog *perf = nullptr)
{
  BLI_assert(resolution >= 2);
  const int side = resolution;
  const int last = side - 1;
  const float uv_step = 2.0f / float(last);
  const auto idx = [side](const int i, const int j) { return i * side + j; };
  const auto qidx = [last](const int i, const int j) { return i * last + j; };

  /* Brush basis in object space: transform local axes through `brush_local_mat_inv`. */
  float3 brush_u_axis(1.0f, 0.0f, 0.0f);
  float3 brush_v_axis(0.0f, 1.0f, 0.0f);
  float3 brush_normal(0.0f, 0.0f, 1.0f);
  mul_mat3_m4_v3(stamp.brush_local_mat_inv.ptr(), brush_u_axis);
  mul_mat3_m4_v3(stamp.brush_local_mat_inv.ptr(), brush_v_axis);
  mul_mat3_m4_v3(stamp.brush_local_mat_inv.ptr(), brush_normal);
  brush_u_axis = math::normalize(brush_u_axis);
  brush_v_axis = math::normalize(brush_v_axis);
  brush_normal = math::normalize(brush_normal);

  /* The brush's sculpt texture is identical for every grid point, so fetch it once here instead of
   * on every sample. When no texture is bound the stamp cannot carry displacement and would collapse
   * onto the brush plane, contributing no geometry — bail out before allocating anything. */
  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);
  if (!mtex->tex) {
    return;
  }

  /* === Pass 1: evaluate the raw VDM displacement for every grid point. === */
  const int grid_size = side * side;
  Array<float3> base_cos(grid_size);
  Array<float3> disp_vecs(grid_size);
  Array<float> active_lens(grid_size);
  /* True unless the grid point sampled pure black -- "nothing painted here" in a VDM texture, see
   * #stamp_displacement -- so its (otherwise garbage) displacement never counts as active. */
  Array<bool> has_data(grid_size, true);

  /* The texture lookup inside #stamp_displacement is the dominant cost here (up to `side * side`
   * samples per stamp). It is thread-safe as long as each task uses its own #tex_pool slot via the
   * task thread id — mirroring the threaded sampling in #calc_brush_texture_colors. Grid points are
   * independent, so evaluate them in parallel. Flat index `n` equals `idx(i, j)`. */
  {
    VDMPerfLog::Clock::time_point t_pass1;
    if (perf) {
      t_pass1 = VDMPerfLog::Clock::now();
    }

    vdm_parallel_for(IndexRange(grid_size), 1024, [&](const IndexRange range, const int task_index) {
      if (perf) {
        VDMPerfLog::record_current_thread(perf->pass1_os_threads_mask);
        VDMPerfLog::record_thread_once(perf->pass1_manual_threads);
      }
      const int thread_id = task_index;
      int64_t local_disp_ns = 0;
      if (perf) {
        /* Atomic max: track the highest thread_id observed across all workers. */
        int cur = perf->pass1_max_thread_id.load(std::memory_order_relaxed);
        while (thread_id > cur &&
               !perf->pass1_max_thread_id.compare_exchange_weak(
                   cur, thread_id, std::memory_order_relaxed))
        {
        }
      }
      for (const int n : range) {
        const int i = n / side;
        const int j = n % side;
        const float u = -1.0f + float(i) * uv_step;
        const float v = -1.0f + float(j) * uv_step;
        const float3 base = stamp_plane_point(stamp, float2(u, v));
        base_cos[n] = base;

        if (perf) {
          const VDMPerfLog::Clock::time_point t_disp = VDMPerfLog::Clock::now();
          disp_vecs[n] = stamp_displacement(stamp,
                                            brush,
                                            mtex,
                                            ss.tex_pool,
                                            base,
                                            thread_id,
                                            use_vector_displacement,
                                            vdm_axis_scale,
                                            has_data[n]);
          local_disp_ns += VDMPerfLog::since(t_disp);
        }
        else {
          disp_vecs[n] = stamp_displacement(stamp,
                                            brush,
                                            mtex,
                                            ss.tex_pool,
                                            base,
                                            thread_id,
                                            use_vector_displacement,
                                            vdm_axis_scale,
                                            has_data[n]);
        }
      }
      if (perf) {
        perf->stamp_disp_ns.fetch_add(local_disp_ns, std::memory_order_relaxed);
      }
    });

    if (perf) {
      perf->pass1_wall_ns += VDMPerfLog::since(t_pass1);
    }
  }

  /* Per-vertex surface normal used to sink the bottom cap and skirt (see below Pass 2). Defaults
   * to the rigid brush normal -- the previous behavior -- for any vertex Pass 1.5 doesn't touch. */
  Array<float3> base_normal(grid_size, brush_normal);

  /* === Pass 1.5: conform the base grid to the sculpted mesh's actual surface. ===
   *
   * #stamp_plane_point() only returns points on the brush's flat tangent plane, so on curved
   * surfaces the generated volume's base does not follow the mesh underneath it. Raycast a coarse
   * subgrid against the mesh to recover the real surface height and normal at a handful of
   * footprint locations, then bilinearly interpolate both fields across the full grid. Footprint
   * locations whose ray misses (or hits something not roughly facing the brush -- see
   * #raycast_stamp_footprint_point) keep the flat plane height and the rigid brush normal,
   * matching the previous behavior there.
   *
   * The texture sampling above already ran against the flat `base_cos`, so nudging `base_cos` here
   * only moves where the sampled displacement is anchored in space -- it does not affect what get
   * sampled from the VDM texture. */
  if (pbvh && pbvh->type() == bke::pbvh::Type::Mesh) {
    VDMPerfLog::Clock::time_point t_surface;
    if (perf) {
      t_surface = VDMPerfLog::Clock::now();
    }

    /* Raycasting is cheap per-sample but #bke::pbvh::raycast() writes into `Node::tmin_` for
     * traversal pruning, so unlike Pass 1 this cannot run on #vdm_parallel_for -- keep the probe
     * grid small and walk it on a single thread. */
    constexpr int max_probe_side = 17;
    const int probe_side = std::min(side, max_probe_side);
    const int probe_last = probe_side - 1;
    const auto probe_idx = [probe_side](const int i, const int j) { return i * probe_side + j; };

    Array<float> probe_height(probe_side * probe_side, 0.0f);
    Array<float3> probe_normal(probe_side * probe_side, brush_normal);
    for (const int pi : IndexRange(probe_side)) {
      const int i = (pi * last) / probe_last;
      for (const int pj : IndexRange(probe_side)) {
        const int j = (pj * last) / probe_last;
        const float3 &plane_point = base_cos[idx(i, j)];
        float3 hit_position;
        float3 hit_normal;
        if (raycast_stamp_footprint_point(*pbvh,
                                          active_mesh,
                                          plane_point,
                                          brush_normal,
                                          stamp.radius,
                                          hit_position,
                                          hit_normal))
        {
          probe_height[probe_idx(pi, pj)] = math::dot(hit_position - plane_point, brush_normal);
          probe_normal[probe_idx(pi, pj)] = hit_normal;
        }
      }
    }

    for (const int i : IndexRange(side)) {
      const float fi = float(i) * float(probe_last) / float(last);
      const int i0 = std::min(int(fi), probe_last);
      const int i1 = std::min(i0 + 1, probe_last);
      const float ti = fi - float(i0);
      for (const int j : IndexRange(side)) {
        const float fj = float(j) * float(probe_last) / float(last);
        const int j0 = std::min(int(fj), probe_last);
        const int j1 = std::min(j0 + 1, probe_last);
        const float tj = fj - float(j0);

        const float h00 = probe_height[probe_idx(i0, j0)];
        const float h10 = probe_height[probe_idx(i1, j0)];
        const float h01 = probe_height[probe_idx(i0, j1)];
        const float h11 = probe_height[probe_idx(i1, j1)];
        const float height = math::interpolate(
            math::interpolate(h00, h10, ti), math::interpolate(h01, h11, ti), tj);

        base_cos[idx(i, j)] += brush_normal * height;

        const float3 &n00 = probe_normal[probe_idx(i0, j0)];
        const float3 &n10 = probe_normal[probe_idx(i1, j0)];
        const float3 &n01 = probe_normal[probe_idx(i0, j1)];
        const float3 &n11 = probe_normal[probe_idx(i1, j1)];
        const float3 blended_normal = math::interpolate(
            math::interpolate(n00, n10, ti), math::interpolate(n01, n11, ti), tj);
        float blended_len;
        const float3 normalized_normal = math::normalize_and_get_length(blended_normal,
                                                                         blended_len);
        base_normal[idx(i, j)] = (blended_len > 0.0f) ? normalized_normal : brush_normal;
      }
    }

    if (perf) {
      perf->surface_conform_ns += VDMPerfLog::since(t_surface);
    }
  }

  VDMPerfLog::Clock::time_point t_baseline;
  if (perf) {
    t_baseline = VDMPerfLog::Clock::now();
  }

  /* No-data points (pure black, see #stamp_displacement) that are also reachable from the grid
   * border through a chain of other no-data points are true texture padding -- unpainted area
   * surrounding the content, which by construction always touches the footprint's edge. Flood-fill
   * from the border rather than treating every no-data point as padding: some VDM textures encode
   * large flat/unraised areas *within* the painted content the same way, and blanket-excluding
   * those fragments the mesh into disconnected islands wherever such an area separates two painted
   * features. */
  Array<bool> is_padding(grid_size, false);
  {
    Vector<int> stack;
    for (const int i : IndexRange(side)) {
      for (const int j : IndexRange(side)) {
        const int n = idx(i, j);
        if ((i == 0 || i == last || j == 0 || j == last) && !has_data[n]) {
          is_padding[n] = true;
          stack.append(n);
        }
      }
    }
    while (!stack.is_empty()) {
      const int n = stack.pop_last();
      const int i = n / side;
      const int j = n % side;
      const int neighbor_i[4] = {i - 1, i + 1, i, i};
      const int neighbor_j[4] = {j, j, j - 1, j + 1};
      for (const int k : IndexRange(4)) {
        const int ni = neighbor_i[k];
        const int nj = neighbor_j[k];
        if (ni < 0 || ni >= side || nj < 0 || nj >= side) {
          continue;
        }
        const int nn = idx(ni, nj);
        if (!has_data[nn] && !is_padding[nn]) {
          is_padding[nn] = true;
          stack.append(nn);
        }
      }
    }
  }

  /* Baseline = mean displacement around the grid border.
   *
   * VDM textures pad their unused area with a flat "neutral" value, but that value is texture
   * dependent (mid-grey, black, ...) and the Sample Bias shifts it further, so it cannot be
   * assumed to be 50 % grey. Measure it directly from the outermost ring of the brush footprint,
   * which by construction lies in that flat padding. This measured baseline is used only to
   * detect the active region below: padding sits near the baseline and is cropped away instead of
   * forming a raised platform. Vertex positions keep the raw displacement untouched, so the
   * generated surface matches what a live VDM Draw stroke produces at the same Strength.
   *
   * Border points flagged as padding above are skipped: their decoded displacement is garbage,
   * not padding-as-neutral-value, and would otherwise pull the whole baseline off. */
  float3 baseline(0.0f);
  int border_num = 0;

  for (const int i : IndexRange(side)) {
    for (const int j : IndexRange(side)) {
      if ((i == 0 || i == last || j == 0 || j == last) && !is_padding[idx(i, j)]) {
        baseline += disp_vecs[idx(i, j)];
        border_num++;
      }
    }
  }
  baseline = (border_num > 0) ? baseline / float(border_num) : float3(0.0f);

  /* Clamp padding to the flat baseline. #active_lens forces it out of the activity test below
   * regardless, but a quad can still turn active from its *other* three corners; without this the
   * padding corner would keep contributing its garbage decoded displacement to that quad's vertex
   * position, producing exactly the boundary spikes this pass exists to prevent. */
  for (const int n : IndexRange(grid_size)) {
    if (is_padding[n]) {
      disp_vecs[n] = baseline;
    }
  }

  if (perf) {
    perf->baseline_ns += VDMPerfLog::since(t_baseline);
  }

  /* Gather the active-region metric (relative to the baseline) and the deepest recess (used to
   * sink the bottom cap below concave geometry). */
  VDMPerfLog::Clock::time_point t_active;
  if (perf) {
    t_active = VDMPerfLog::Clock::now();
  }

  float min_depth = 0.0f;
  for (const int n : IndexRange(grid_size)) {
    /* Padding is already clamped to the baseline above, but skip it explicitly: this is what
     * actually excludes unpainted texture area from the active region, rather than relying solely
     * on proximity to a baseline that unpainted corners could otherwise have skewed. */
    if (is_padding[n]) {
      active_lens[n] = 0.0f;
      continue;
    }
    /* Activity is measured relative to the baseline so flat padding is cropped, but the vertex
     * positions keep the raw displacement so the generated surface matches the live VDM stroke. */
    active_lens[n] = math::length(disp_vecs[n] - baseline);
    min_depth = math::min(min_depth, math::dot(disp_vecs[n], brush_normal));
  }

  if (perf) {
    perf->active_compute_ns += VDMPerfLog::since(t_active);
  }

  /* Sink the bottom cap if the VDM has a recess below the brush plane. Only the depth is a single
   * value shared by the whole stamp (the deepest recess anywhere in the footprint); the direction
   * it is sunk in follows each vertex's own #base_normal so the bottom -- and the skirt walls
   * connecting it to the top -- keep hugging the conformed surface near the footprint's edges
   * instead of sinking along one rigid direction. */
  const float bottom_sink = math::min(min_depth, 0.0f);

  /* === Pass 2: determine which quads carry meaningful displacement. ===
   * Threshold = 1 % of the grid cell size — separates real content from 8-bit quantisation
   * noise and floating-point rounding in truly-flat border areas. */
  const float cell_size = (2.0f * stamp.radius) / float(last);
  const float threshold = cell_size * 0.01f;
  const int quad_count = last * last;
  Array<bool> quad_active(quad_count, false);

  VDMPerfLog::Clock::time_point t_quad;
  if (perf) {
    t_quad = VDMPerfLog::Clock::now();
  }

  for (const int i : IndexRange(last)) {
    for (const int j : IndexRange(last)) {
      quad_active[qidx(i, j)] = active_lens[idx(i, j)] > threshold ||
                                 active_lens[idx(i + 1, j)] > threshold ||
                                 active_lens[idx(i + 1, j + 1)] > threshold ||
                                 active_lens[idx(i, j + 1)] > threshold;
    }
  }

  if (perf) {
    perf->quad_mark_ns += VDMPerfLog::since(t_quad);
    perf->total_quads += quad_count;
    for (const int q : IndexRange(quad_count)) {
      if (quad_active[q]) {
        perf->active_quads++;
      }
    }
  }

  /* === Pass 3: allocate mesh vertices only for vertices that belong to at least one active quad. === */
  VDMPerfLog::Clock::time_point t_vert;
  int verts_before = 0;
  if (perf) {
    t_vert = VDMPerfLog::Clock::now();
    verts_before = int(positions.size());
  }

  Array<bool> vert_needed(grid_size, false);
  for (const int i : IndexRange(last)) {
    for (const int j : IndexRange(last)) {
      if (!quad_active[qidx(i, j)]) {
        continue;
      }
      vert_needed[idx(i, j)] = true;
      vert_needed[idx(i + 1, j)] = true;
      vert_needed[idx(i + 1, j + 1)] = true;
      vert_needed[idx(i, j + 1)] = true;
    }
  }

  Array<int> top_verts(grid_size, -1);
  Array<int> bottom_verts(grid_size, -1);
  for (const int n : IndexRange(grid_size)) {
    if (!vert_needed[n]) {
      continue;
    }
    top_verts[n] = int(positions.append_and_get_index(base_cos[n] + disp_vecs[n]));
    bottom_verts[n] = int(
        positions.append_and_get_index(base_cos[n] + base_normal[n] * bottom_sink));
  }

  if (perf) {
    perf->vert_alloc_ns += VDMPerfLog::since(t_vert);
    perf->verts_emitted += int(positions.size()) - verts_before;
  }

  const auto add_quad_oriented = [&](int v0, int v1, int v2, int v3, const float3 &outward) {
    const float3 normal = math::cross(positions[v1] - positions[v0], positions[v2] - positions[v0]);
    if (math::dot(normal, outward) < 0.0f) {
      std::swap(v1, v3);
    }
    face_offsets.append(corner_verts.size());
    corner_verts.append(v0);
    corner_verts.append(v1);
    corner_verts.append(v2);
    corner_verts.append(v3);
  };

  /* === Pass 4: build faces. ===
   * Top and bottom cap for every active quad. Skirt on every edge that borders an inactive quad
   * or the grid boundary — this guarantees a watertight volume regardless of the active region's
   * shape (convex, concave, non-rectangular). */
  VDMPerfLog::Clock::time_point t_face;
  int faces_before = 0;
  if (perf) {
    t_face = VDMPerfLog::Clock::now();
    faces_before = int(face_offsets.size());
  }

  for (const int i : IndexRange(last)) {
    for (const int j : IndexRange(last)) {
      if (!quad_active[qidx(i, j)]) {
        continue;
      }

      add_quad_oriented(top_verts[idx(i, j)],
                        top_verts[idx(i + 1, j)],
                        top_verts[idx(i + 1, j + 1)],
                        top_verts[idx(i, j + 1)],
                        brush_normal);
      add_quad_oriented(bottom_verts[idx(i, j)],
                        bottom_verts[idx(i, j + 1)],
                        bottom_verts[idx(i + 1, j + 1)],
                        bottom_verts[idx(i + 1, j)],
                        -brush_normal);

      /* Skirt: emit a wall on each edge that faces outside the active region. */
      if (j == 0 || !quad_active[qidx(i, j - 1)]) {
        add_quad_oriented(top_verts[idx(i, j)],
                          top_verts[idx(i + 1, j)],
                          bottom_verts[idx(i + 1, j)],
                          bottom_verts[idx(i, j)],
                          -brush_v_axis);
      }
      if (j == last - 1 || !quad_active[qidx(i, j + 1)]) {
        add_quad_oriented(top_verts[idx(i, j + 1)],
                          bottom_verts[idx(i, j + 1)],
                          bottom_verts[idx(i + 1, j + 1)],
                          top_verts[idx(i + 1, j + 1)],
                          brush_v_axis);
      }
      if (i == 0 || !quad_active[qidx(i - 1, j)]) {
        add_quad_oriented(top_verts[idx(i, j + 1)],
                          top_verts[idx(i, j)],
                          bottom_verts[idx(i, j)],
                          bottom_verts[idx(i, j + 1)],
                          -brush_u_axis);
      }
      if (i == last - 1 || !quad_active[qidx(i + 1, j)]) {
        add_quad_oriented(top_verts[idx(i + 1, j)],
                          top_verts[idx(i + 1, j + 1)],
                          bottom_verts[idx(i + 1, j + 1)],
                          bottom_verts[idx(i + 1, j)],
                          brush_u_axis);
      }
    }
  }

  if (perf) {
    perf->face_build_ns += VDMPerfLog::since(t_face);
    perf->faces_emitted += int(face_offsets.size()) - faces_before;
  }
}

/**
 * Build a Mesh containing every captured stamp volume.
 *
 * The result holds only the stamp geometry. The "into active mesh" mode concatenates it onto the
 * active mesh afterwards with a specialized mesh append, which is far cheaper than round-tripping the whole
 * active mesh through a BMesh just to append loose geometry.
 */
static Mesh *build_stamps_mesh(const Span<VDMStampData> stamps,
                               const Brush &brush,
                               SculptSession &ss,
                               bke::pbvh::Tree *pbvh,
                               const Mesh &active_mesh,
                               const int resolution,
                               VDMPerfLog *perf = nullptr)
{
  const bool use_vector_displacement = brush_uses_vector_displacement(brush);
  const int grid_size = resolution * resolution;
  const int quad_count = (resolution - 1) * (resolution - 1);
  const int expected_face_count = quad_count * 2 + (resolution - 1) * 4;
  const int expected_loop_count = expected_face_count * 4;
  const int stamps_num = int(stamps.size());

  Vector<float3> positions;
  Vector<int> face_offsets;
  Vector<int> corner_verts;
  positions.reserve(stamps_num * grid_size * 2);
  face_offsets.reserve(stamps_num * expected_face_count);
  corner_verts.reserve(stamps_num * expected_loop_count);

  const float3 vdm_axis_scale(math::safe_divide(1.0f, pow2f(brush.mtex.size[0])),
                              math::safe_divide(1.0f, pow2f(brush.mtex.size[1])),
                              math::safe_divide(1.0f, pow2f(brush.mtex.size[2])));

  VDMPerfLog::Clock::time_point t_build;
  if (perf) {
    t_build = VDMPerfLog::Clock::now();
  }

  for (const VDMStampData &stamp : stamps) {
    add_stamp_to_mesh_buffers(positions,
                              face_offsets,
                              corner_verts,
                              stamp,
                              brush,
                              ss,
                              pbvh,
                              active_mesh,
                              resolution,
                              use_vector_displacement,
                              vdm_axis_scale,
                              perf);
  }

  if (perf) {
    perf->build_mesh_ns += VDMPerfLog::since(t_build);
  }

  Mesh *mesh = BKE_mesh_new_nomain(positions.size(), 0, face_offsets.size(), corner_verts.size());
  mesh->vert_positions_for_write().copy_from(positions.as_span());
  /* A stamp whose displacement stays below the activity threshold produces no active quads, and
   * hence no faces. The face-offset array is then empty, so writing the trailing offset via #last()
   * would index out of bounds; only populate the offsets when at least one face exists. */
  if (!face_offsets.is_empty()) {
    mesh->face_offsets_for_write().drop_back(1).copy_from(face_offsets.as_span());
    mesh->face_offsets_for_write().last() = corner_verts.size();
  }
  mesh->corner_verts_for_write().copy_from(corner_verts.as_span());

  bke::mesh_calc_edges(*mesh, false, false);
  /* Smooth-shade the displaced surface while keeping the volume's hard corners crisp. The fresh
   * mesh carries no `sharp_face` attribute, so every face shades smooth; only edges whose dihedral
   * angle exceeds ~30 degrees — the top/skirt/bottom boundaries — are then marked sharp. Edges must
   * exist first, hence this runs after #mesh_calc_edges. */
  bke::mesh_sharp_edges_set_from_angle(*mesh, DEG2RADF(30.0f));
  return mesh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator
 * \{ */

static wmOperatorStatus insert_mesh_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = CTX_data_active_object(C);
  View3D *v3d = CTX_wm_view3d(C);
  Scene *scene = CTX_data_scene(C);
  SculptSession &ss = *ob->runtime->sculpt_session;

  /* Dyntopo keeps geometry inside a BMesh; the regular mesh path below does not apply. */
  if (ss.bm) {
    ss.vdm_stamps.clear();
    return OPERATOR_CANCELLED;
  }
  if (ss.vdm_stamps.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  VDMPerfLog perf;
  VDMPerfLog *perf_ptr = VDM_PERF_LOG_ENABLED ? &perf : nullptr;
  if (perf_ptr) {
    perf_ptr->scheduler_threads = BLI_task_scheduler_num_threads();
  }
  VDMPerfLog::Clock::time_point t_exec;
  if (perf_ptr) {
    t_exec = VDMPerfLog::Clock::now();
  }

  const Brush &brush = *BKE_paint_brush_for_read(&scene->toolsettings->sculpt->paint);
  const bool into_active = (brush.flag2 & BRUSH_INSERT_INTO_ACTIVE) != 0;

  const Vector<VDMStampData> stamps = ss.vdm_stamps;
  ss.vdm_stamps.clear();

  Mesh &active_mesh = *id_cast<Mesh *>(ob->data);
  /* Derive resolution from the mesh density at the first stamp's footprint so the generated grid
   * matches the polygon size of the target mesh rather than using an arbitrary fixed value.
   * Non-const: also used to raycast the stamp footprint against the surface for #add_stamp_to_mesh_buffers's
   * surface-conform pass. */
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*ob);

  {
    VDMPerfLog::Clock::time_point t_res;
    if (perf_ptr) {
      t_res = VDMPerfLog::Clock::now();
    }
    int resolution = resolution_from_mesh_density(
        active_mesh, pbvh, stamps[0].location, stamps[0].radius, perf_ptr);
    /* Quality scales the density-derived resolution: Low halves it, High doubles it. Medium keeps
     * the reference-mesh density. #resolution_from_mesh_density already clamps its result to 256, so
     * High must allow up to 512 (the operator's resolution maximum); otherwise a dense reference
     * mesh saturates Medium at 256 and High cannot rise above it. */
    switch (brush.vdm_insert_quality) {
      case BRUSH_VDM_INSERT_QUALITY_LOW:
        resolution = math::clamp(resolution / 2, 2, 512);
        break;
      case BRUSH_VDM_INSERT_QUALITY_HIGH:
        resolution = math::clamp(resolution * 2, 2, 512);
        break;
      case BRUSH_VDM_INSERT_QUALITY_MEDIUM:
        break;
    }
    if (perf_ptr) {
      perf_ptr->resolution_ns = VDMPerfLog::since(t_res);
      perf_ptr->resolution = resolution;
      perf_ptr->stamps_num = int(stamps.size());
    }

    if (into_active) {
      /* Merge the stamps into the active mesh, then write it back. The geometry change happens while a
       * sculpt session is live, so wrap it in a geometry undo step and drop the cached PBVH afterwards
       * (see #SCULPT_OT_paint_mask_slice for the same pattern). */
      undo::geometry_begin(*scene, *ob, op);

      /* Build only the (small) stamp geometry and concatenate it onto the active mesh with a
       * specialized append. The stamps are loose geometry that is simply appended, so converting the
       * whole active mesh (millions of polygons) to a BMesh and back just to add them is unnecessary. */
      Mesh *stamp_mesh = build_stamps_mesh(
          stamps, brush, ss, pbvh, active_mesh, resolution, perf_ptr);

      if (perf_ptr) {
        perf_ptr->join_active_verts = active_mesh.verts_num;
        perf_ptr->join_active_faces = active_mesh.faces_num;
        perf_ptr->join_stamp_verts = stamp_mesh->verts_num;
        perf_ptr->join_stamp_faces = stamp_mesh->faces_num;
      }

      VDMPerfLog::Clock::time_point t_join;
      if (perf_ptr) {
        t_join = VDMPerfLog::Clock::now();
      }

      const int base_faces = active_mesh.faces_num;
      const int append_faces = stamp_mesh->faces_num;
      const bool active_has_face_set = bool(active_mesh.attributes().lookup<int>(".sculpt_face_set"));
      const bool stamp_has_face_set = bool(stamp_mesh->attributes().lookup<int>(".sculpt_face_set"));
      const int new_stamp_face_set = (append_faces > 0 && active_has_face_set && !stamp_has_face_set) ?
                                         face_set::find_next_available_id(active_mesh) :
                                         0;

      VDMPerfLog::Clock::time_point t_append;
      if (perf_ptr) {
        t_append = VDMPerfLog::Clock::now();
      }
      bke::mesh_append(active_mesh, *stamp_mesh);
      if (new_stamp_face_set != 0) {
        bke::SpanAttributeWriter<int> face_sets =
            active_mesh.attributes_for_write().lookup_for_write_span<int>(".sculpt_face_set");
        face_sets.span.slice(base_faces, append_faces).fill(new_stamp_face_set);
        face_sets.finish();
      }
      if (perf_ptr) {
        perf_ptr->join_append_ns = VDMPerfLog::since(t_append);
      }

      BKE_id_free(bmain, stamp_mesh);

      if (perf_ptr) {
        perf_ptr->join_nomain_to_mesh_ns = 0;
        perf_ptr->join_ns = VDMPerfLog::since(t_join);
      }

      undo::geometry_end(*ob);
      BKE_sculptsession_free_pbvh(*ob);

      BKE_mesh_batch_cache_dirty_tag(&active_mesh, BKE_MESH_BATCH_DIRTY_ALL);
      DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
      WM_event_add_notifier(C, NC_GEOM | ND_DATA, &active_mesh);

      if (perf_ptr) {
        perf_ptr->total_exec_ns = VDMPerfLog::since(t_exec);
        perf_ptr->print();
      }
      return OPERATOR_FINISHED;
    }

    /* Separate object: build the stamps in their own mesh. */
    Mesh *new_mesh = build_stamps_mesh(
        stamps, brush, ss, pbvh, active_mesh, resolution, perf_ptr);

    /* `add_type` only applies the active object's location and rotation, so bake its scale into the
     * geometry to keep the stamp aligned with where the stroke happened. */
    for (float3 &position : new_mesh->vert_positions_for_write()) {
      position *= ob->scale;
    }

    if (!new_mesh || new_mesh->verts_num == 0) {
      if (new_mesh) {
        BKE_id_free(nullptr, new_mesh);
      }
      if (perf_ptr) {
        perf_ptr->total_exec_ns = VDMPerfLog::since(t_exec);
        perf_ptr->print();
      }
      return OPERATOR_CANCELLED;
    }

    ushort local_view_bits = 0;
    if (v3d && v3d->localvd) {
      local_view_bits = v3d->local_view_uid;
    }

    Object *new_ob = ed::object::add_type(
        C, OB_MESH, nullptr, ob->loc, ob->rot, false, local_view_bits);
    BKE_mesh_nomain_to_mesh(new_mesh, id_cast<Mesh *>(new_ob->data), new_ob);

    WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, new_ob);
    BKE_mesh_batch_cache_dirty_tag(id_cast<Mesh *>(new_ob->data), BKE_MESH_BATCH_DIRTY_ALL);
    DEG_relations_tag_update(bmain);
    DEG_id_tag_update(&new_ob->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, new_ob->data);

    /* `add_type` activates the freshly created object, which is in Object Mode, dropping the viewport
     * out of Sculpt Mode. Re-activate the original sculpted object's base so the user stays in Sculpt
     * Mode on the same object; the new object remains in the scene, selected but not active. */
    ViewLayer *view_layer = CTX_data_view_layer(C);
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    if (Base *orig_base = BKE_view_layer_base_find(view_layer, ob)) {
      ed::object::base_activate(C, orig_base);
    }
  }

  if (perf_ptr) {
    perf_ptr->total_exec_ns = VDMPerfLog::since(t_exec);
    perf_ptr->print();
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_insert_mesh(wmOperatorType *ot)
{
  ot->name = "Insert Mesh";
  ot->description = "Create a watertight mesh from brush stamps (vector displacement or alpha texture)";
  ot->idname = "SCULPT_OT_insert_mesh";

  ot->poll = insert_mesh_poll;
  ot->exec = insert_mesh_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "resolution",
              32,
              2,
              512,
              "Resolution",
              "Grid resolution for the generated mesh",
              2,
              256);
}

/** \} */

}  // namespace blender::ed::sculpt_paint
