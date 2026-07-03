/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <unordered_map>

#include "MEM_guardedalloc.h"

#include "optimizer.hpp"
#include "parametrizer.hpp"
#include "quadriflow_capi.hpp"

using namespace qflow;

struct ObjVertex {
  uint32_t p = (uint32_t)-1;
  uint32_t n = (uint32_t)-1;
  uint32_t uv = (uint32_t)-1;

  ObjVertex() = default;

  ObjVertex(uint32_t pi)
  {
    p = pi;
  }

  bool operator==(const ObjVertex &v) const
  {
    return v.p == p && v.n == n && v.uv == uv;
  }
};

struct ObjVertexHash {
  std::size_t operator()(const ObjVertex &v) const
  {
    size_t hash = std::hash<uint32_t>()(v.p);
    hash = hash * 37 + std::hash<uint32_t>()(v.uv);
    hash = hash * 37 + std::hash<uint32_t>()(v.n);
    return hash;
  }
};

using VertexMap = std::unordered_map<ObjVertex, uint32_t, ObjVertexHash>;

static int check_if_canceled(float progress,
                             void (*update_cb)(void *, float progress, int *cancel),
                             void *update_cb_data)
{
  int cancel = 0;
  update_cb(update_cb_data, progress, &cancel);
  return cancel;
}

void QFLOW_quadriflow_remesh(QuadriflowRemeshData *qrd,
                             void (*update_cb)(void *, float progress, int *cancel),
                             void *update_cb_data)
{
  Parametrizer field;
  VertexMap vertexMap;

  /* Get remeshing parameters. */
  int faces = qrd->target_faces;

  if (qrd->preserve_sharp) {
    field.flag_preserve_sharp = 1;
  }
  if (qrd->preserve_boundary) {
    field.flag_preserve_boundary = 1;
  }
  if (qrd->adaptive_scale) {
    field.flag_adaptive_scale = 1;
  }
  if (qrd->minimum_cost_flow) {
    field.flag_minimum_cost_flow = 1;
  }
  if (qrd->aggresive_sat) {
    field.flag_aggresive_sat = 1;
  }
  if (qrd->rng_seed) {
    field.hierarchy.rng_seed = qrd->rng_seed;
  }

  if (check_if_canceled(0.0f, update_cb, update_cb_data) != 0) {
    return;
  }

  /* Copy mesh to quadriflow data structures. */
  std::vector<Vector3d> positions;
  std::vector<uint32_t> indices;
  std::vector<ObjVertex> vertices;

  for (int i = 0; i < qrd->totverts; i++) {
    Vector3d v(qrd->verts[i * 3], qrd->verts[i * 3 + 1], qrd->verts[i * 3 + 2]);
    positions.push_back(v);
  }

  for (int q = 0; q < qrd->totfaces; q++) {
    Vector3i f(qrd->faces[q * 3], qrd->faces[q * 3 + 1], qrd->faces[q * 3 + 2]);

    ObjVertex tri[6];
    int nVertices = 3;

    tri[0] = ObjVertex(f[0]);
    tri[1] = ObjVertex(f[1]);
    tri[2] = ObjVertex(f[2]);

    for (int i = 0; i < nVertices; ++i) {
      const ObjVertex &v = tri[i];
      VertexMap::const_iterator it = vertexMap.find(v);
      if (it == vertexMap.end()) {
        vertexMap[v] = (uint32_t)vertices.size();
        indices.push_back((uint32_t)vertices.size());
        vertices.push_back(v);
      }
      else {
        indices.push_back(it->second);
      }
    }
  }

  field.F.resize(3, indices.size() / 3);
  memcpy(field.F.data(), indices.data(), sizeof(uint32_t) * indices.size());

  field.V.resize(3, vertices.size());
  for (uint32_t i = 0; i < vertices.size(); ++i) {
    field.V.col(i) = positions.at(vertices[i].p);
  }

  if (check_if_canceled(0.1f, update_cb, update_cb_data)) {
    return;
  }

  /* Start processing the input mesh data */
  field.NormalizeMesh();
  field.Initialize(faces);

  if (check_if_canceled(0.2f, update_cb, update_cb_data)) {
    return;
  }

  /* Setup mesh boundary constraints if needed */
  if (field.flag_preserve_boundary) {
    Hierarchy &mRes = field.hierarchy;
    mRes.clearConstraints();
    for (uint32_t i = 0; i < 3 * mRes.mF.cols(); ++i) {
      if (mRes.mE2E[i] == -1) {
        uint32_t i0 = mRes.mF(i % 3, i / 3);
        uint32_t i1 = mRes.mF((i + 1) % 3, i / 3);
        Vector3d p0 = mRes.mV[0].col(i0), p1 = mRes.mV[0].col(i1);
        Vector3d edge = p1 - p0;
        if (edge.squaredNorm() > 0) {
          edge.normalize();
          mRes.mCO[0].col(i0) = p0;
          mRes.mCO[0].col(i1) = p1;
          mRes.mCQ[0].col(i0) = mRes.mCQ[0].col(i1) = edge;
          mRes.mCQw[0][i0] = mRes.mCQw[0][i1] = mRes.mCOw[0][i0] = mRes.mCOw[0][i1] = 1.0;
        }
      }
    }
    mRes.propagateConstraints();
  }

  /* Apply optional per-vertex orientation guidance (curvature / guide curves).
   * The cross field smoothing in `optimize_orientations` blends each vertex
   * toward `mCQ` by weight `mCQw`, so writing the guide here biases the edge
   * flow. The guide arrays are indexed by input vertex; map them to the
   * internal vertex order through `vertices[i].p`. Subdivision in `Initialize`
   * only appends vertices, so the first `vertices.size()` columns keep their
   * original order and any appended vertices stay unconstrained. */
  if (qrd->guide_dirs && qrd->guide_weights) {
    Hierarchy &mRes = field.hierarchy;
    /* The boundary path above already sized and filled the constraints; size
     * them here when it did not run so `optimize_orientations` reads them. */
    if (!field.flag_preserve_boundary) {
      mRes.clearConstraints();
    }
    const MatrixXd &N0 = mRes.mN[0];
    for (uint32_t i = 0; i < vertices.size(); ++i) {
      const uint32_t orig = vertices[i].p;
      const float w = qrd->guide_weights[orig];
      if (w <= 0.0f) {
        continue;
      }
      /* Project the object-space guide onto the vertex tangent plane; the
       * cross field is defined there. */
      Vector3d dir(qrd->guide_dirs[orig * 3 + 0],
                   qrd->guide_dirs[orig * 3 + 1],
                   qrd->guide_dirs[orig * 3 + 2]);
      const Vector3d n = N0.col(i);
      dir -= n * n.dot(dir);
      if (dir.squaredNorm() < 1e-12) {
        continue;
      }
      dir.normalize();
      /* Keep the stronger constraint when one already exists (e.g. boundary). */
      if (double(w) > mRes.mCQw[0][i]) {
        mRes.mCQ[0].col(i) = dir;
        mRes.mCQw[0][i] = w;
      }
      /* Positional pins are only valid along feature lines (e.g. face set
       * boundaries): `optimize_positions` removes the `mCQ` component of the
       * pull, so a pinned vertex still slides along the guide direction, which
       * matches how `flag_preserve_boundary` keeps open boundaries in place.
       * Pinning the whole surface would fight the uniform parametrization, so
       * a separate sparse weight array controls this. */
      if (qrd->guide_pin_weights) {
        const float pin_w = qrd->guide_pin_weights[orig];
        if (double(pin_w) > mRes.mCOw[0][i]) {
          mRes.mCO[0].col(i) = mRes.mV[0].col(i);
          mRes.mCOw[0][i] = pin_w;
        }
      }
    }
    mRes.propagateConstraints();
  }

  /* Optimize the mesh field orientations (tangental field etc) */
  Optimizer::optimize_orientations(field.hierarchy);
  field.ComputeOrientationSingularities();

  if (check_if_canceled(0.3f, update_cb, update_cb_data)) {
    return;
  }

  if (field.flag_adaptive_scale == 1) {
    field.EstimateSlope();
  }

  if (check_if_canceled(0.4f, update_cb, update_cb_data)) {
    return;
  }

  Optimizer::optimize_scale(field.hierarchy, field.rho, field.flag_adaptive_scale);
  field.flag_adaptive_scale = 1;

  /* Override the per-vertex scale field with the caller-provided density.
   * `optimize_scale` above filled `mS` with 1 (the upstream adaptive path is
   * effectively dead: its curvature source `rho` is hard-coded to 1), and
   * `flag_adaptive_scale` is already forced on, so every downstream position
   * and integer-grid solve honors `mS` as a per-vertex multiplier of the
   * global target edge length `mScale`. */
  if (qrd->guide_scales) {
    Hierarchy &mRes = field.hierarchy;
    MatrixXd &S = mRes.mS[0];
    const int cols = int(S.cols());
    std::vector<char> assigned(cols, 0);
    for (uint32_t i = 0; i < vertices.size(); ++i) {
      const float s = qrd->guide_scales[vertices[i].p];
      if (s > 0.0f) {
        S(0, i) = double(s);
        S(1, i) = double(s);
        assigned[i] = 1;
      }
    }
    /* Vertices appended by the subdivision in `Initialize` have no source
     * value; diffuse from assigned neighbors. A split vertex always neighbors
     * original vertices, so a few sweeps cover chained splits. The staged
     * mark keeps each sweep order-independent. */
    for (int sweep = 0; sweep < 10; ++sweep) {
      bool changed = false;
      for (int i = 0; i < cols; ++i) {
        if (assigned[i]) {
          continue;
        }
        double sum = 0.0;
        int num = 0;
        for (const auto &link : mRes.mAdj[0][i]) {
          if (assigned[link.id] == 1) {
            sum += S(0, link.id);
            num++;
          }
        }
        if (num > 0) {
          S(0, i) = S(1, i) = sum / num;
          assigned[i] = 2;
          changed = true;
        }
      }
      for (int i = 0; i < cols; ++i) {
        if (assigned[i] == 2) {
          assigned[i] = 1;
        }
      }
      if (!changed) {
        break;
      }
    }
    /* Propagate to the coarser multigrid levels, mirroring the propagation at
     * the end of `optimize_scale`. */
    for (size_t l = 0; l + 1 < mRes.mS.size(); ++l) {
      const MatrixXd &S_level = mRes.mS[l];
      MatrixXd &S_next = mRes.mS[l + 1];
      const MatrixXi &toUpper = mRes.mToUpper[l];
      for (int i = 0; i < toUpper.cols(); ++i) {
        Vector2i upper = toUpper.col(i);
        Vector2d s0 = S_level.col(upper[0]);
        if (upper[1] != -1) {
          s0 = (s0 + Vector2d(S_level.col(upper[1]))) * 0.5;
        }
        S_next.col(i) = s0;
      }
    }
  }

  Optimizer::optimize_positions(field.hierarchy, field.flag_adaptive_scale);

  field.ComputePositionSingularities();

  if (check_if_canceled(0.5f, update_cb, update_cb_data)) {
    return;
  }

  /* Compute the final quad geometry using a maxflow solver */
  if (!field.ComputeIndexMap()) {
    /* Error computing the result. */
    return;
  }

  if (check_if_canceled(0.9f, update_cb, update_cb_data)) {
    return;
  }

  /* Get the output mesh data */
  qrd->out_totverts = field.O_compact.size();
  qrd->out_totfaces = field.F_compact.size();

  qrd->out_verts = MEM_new_array_uninitialized<float>(qrd->out_totverts * 3, __func__);
  qrd->out_faces = MEM_new_array_uninitialized<int>(qrd->out_totfaces * 4, __func__);

  for (int i = 0; i < qrd->out_totverts; i++) {
    auto t = field.O_compact[i] * field.normalize_scale + field.normalize_offset;
    qrd->out_verts[i * 3] = t[0];
    qrd->out_verts[i * 3 + 1] = t[1];
    qrd->out_verts[i * 3 + 2] = t[2];
  }

  for (int i = 0; i < qrd->out_totfaces; i++) {
    qrd->out_faces[i * 4] = field.F_compact[i][0];
    qrd->out_faces[i * 4 + 1] = field.F_compact[i][1];
    qrd->out_faces[i * 4 + 2] = field.F_compact[i][2];
    qrd->out_faces[i * 4 + 3] = field.F_compact[i][3];
  }
}
