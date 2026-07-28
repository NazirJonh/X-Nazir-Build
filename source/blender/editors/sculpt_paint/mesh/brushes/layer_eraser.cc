/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_multires.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_sculpt_layers.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint::brushes {

inline namespace layer_eraser_cc {

struct LocalData {
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<float3> translations;
};

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const MeshAttributeData &attribute_data,
                       const Span<float3> vert_normals,
                       const Span<float3> layer_data,
                       const float layer_effective,
                       const float strength,
                       const bke::pbvh::MeshNode &node,
                       Object &object,
                       LocalData &tls,
                       const PositionDeformData &position_data)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const Span<int> verts = node.verts();

  calc_factors_common_mesh_indexed(depsgraph,
                                   brush,
                                   object,
                                   attribute_data,
                                   position_data.eval,
                                   vert_normals,
                                   node,
                                   tls.factors,
                                   tls.distances);
  scale_factors(tls.factors, strength);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  for (const int i : verts.index_range()) {
    translations[i] = -layer_data[verts[i]] * layer_effective;
  }
  scale_translations(translations, tls.factors);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const Span<float3> contribution,
                       const float layer_effective,
                       const float strength,
                       const bke::pbvh::GridsNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

  const Span<int> grids = node.grids();
  const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  calc_factors_common_grids(depsgraph, brush, object, positions, node, tls.factors, tls.distances);
  scale_factors(tls.factors, strength);

  tls.translations.resize(positions.size());
  const MutableSpan<float3> translations = tls.translations;
  for (const int i : grids.index_range()) {
    const IndexRange node_range = bke::ccg::grid_range(key, i);
    const IndexRange grid_range = bke::ccg::grid_range(key, grids[i]);
    for (const int offset : IndexRange(key.grid_area)) {
      translations[node_range[offset]] = -contribution[grid_range[offset]] * layer_effective;
    }
  }
  scale_translations(translations, tls.factors);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

}  // namespace layer_eraser_cc

void do_layer_eraser_brush(const Depsgraph &depsgraph,
                           const Sculpt &sd,
                           Object &object,
                           const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh || pbvh->type() == bke::pbvh::Type::BMesh) {
    return;
  }

  Mesh &mesh = *id_cast<Mesh *>(object.data);
  SculptLayer *layer = bke::sculpt_layers::active_get(mesh);
  if (!layer || layer->domain != layers::domain_for(object)) {
    return;
  }

  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const float strength = std::min(ss.cache->bstrength, 1.0f);
  const float layer_effective = bke::sculpt_layers::effective(*layer);

  threading::EnumerableThreadSpecific<LocalData> all_tls;

  if (pbvh->type() == bke::pbvh::Type::Mesh) {
    const Span<float3> layer_data = bke::sculpt_layers::data_ensure(*layer,
                                                                    layers::element_count(object));
    const MeshAttributeData attribute_data(mesh);
    const PositionDeformData position_data(depsgraph, object);
    const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);
    MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
    node_mask.foreach_index(
        [&](const int i) {
          LocalData &tls = all_tls.local();
          calc_faces(depsgraph,
                    sd,
                    brush,
                    attribute_data,
                    vert_normals,
                    layer_data,
                    layer_effective,
                    strength,
                    nodes[i],
                    object,
                    tls,
                    position_data);
          bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[i]);
        },
        exec_mode::grain_size(1));
  }
  else if (pbvh->type() == bke::pbvh::Type::Grids && ss.subdiv_ccg) {
    SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
    if (ss.cache->layer_eraser.object_space_contribution.is_empty()) {
      ss.cache->layer_eraser.object_space_contribution.reinitialize(subdiv_ccg.positions.size());
      ss.cache->layer_eraser.object_space_contribution.fill(float3(0.0f));
      BKE_multires_sculpt_layer_object_contribution(
          mesh, subdiv_ccg, *layer, ss.cache->layer_eraser.object_space_contribution);
    }
    const Span<float3> contribution = ss.cache->layer_eraser.object_space_contribution;
    MutableSpan<float3> positions = subdiv_ccg.positions;
    MutableSpan<bke::pbvh::GridsNode> nodes = pbvh->nodes<bke::pbvh::GridsNode>();
    node_mask.foreach_index(
        [&](const int i) {
          LocalData &tls = all_tls.local();
          calc_grids(
              depsgraph, sd, object, brush, contribution, layer_effective, strength, nodes[i], tls);
          bke::pbvh::update_node_bounds_grids(subdiv_ccg.grid_area, positions, nodes[i]);
        },
        exec_mode::grain_size(1));
  }
  else {
    return;
  }

  pbvh->tag_positions_changed(node_mask);
  pbvh->flush_bounds_to_parents();
}

}  // namespace blender::ed::sculpt_paint::brushes
