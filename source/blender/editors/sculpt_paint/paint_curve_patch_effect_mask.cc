/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Sculpt mask target for a Curve Patch session. The target is kept separate from the relief
 * effect: a mask changes an attribute, not vertex positions, and therefore needs its own snapshot,
 * restore and undo handling.
 */

#include <algorithm>
#include <optional>
#include <utility>

#include "paint_curve_patch_effect.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_types.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_bit_vector.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_execution_mode.hh"
#include "BLI_index_mask.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "paint_curve_patch_effect_common.hh"
#include "paint_intern.hh"

#include "mesh/sculpt_intern.hh"
#include "mesh/sculpt_undo.hh"

namespace blender::ed::sculpt_paint {

namespace {

class MaskEffect : public CurvePatchEffect {
 public:
  UpdateType update_type() const override
  {
    return UpdateType::Mask;
  }
  int64_t element_num(Object &ob) const override;
  void restore(Object &ob, const CurvePatchSession &patch) override;
  void begin_restamp(const Depsgraph &depsgraph, Object &ob, CurvePatchSession &patch) override;
  void apply_pass(const Depsgraph &depsgraph,
                  Object &ob,
                  const Brush &brush,
                  CurvePatchSession &patch,
                  const CurvePatchItem &item) override;
  void end_restamp(Object &ob, CurvePatchSession &patch) override;
  void commit(const Scene &scene,
              const Depsgraph &depsgraph,
              Object &ob,
              const CurvePatchSession &patch) override;
  int64_t snapshot_size() const override
  {
    return orig_masks_.size();
  }

 private:
  Map<int, float> orig_masks_;
};

int64_t MaskEffect::element_num(Object &ob) const
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      return id_cast<Mesh *>(ob.data)->verts_num;
    case bke::pbvh::Type::Grids:
      return ob.runtime->sculpt_session->subdiv_ccg->masks.size();
    case bke::pbvh::Type::BMesh:
      return 0;
  }
  return 0;
}

void MaskEffect::restore(Object &ob, const CurvePatchSession &patch)
{
  if (orig_masks_.is_empty()) {
    return;
  }

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  IndexMaskMemory memory;
  const IndexMask node_mask = IndexMask::from_bits(patch.apply.last_restamp_nodes, memory);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *id_cast<Mesh *>(ob.data);
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      bke::SpanAttributeWriter<float> masks = attributes.lookup_for_write_span<float>(
          ".sculpt_mask");
      if (!masks) {
        return;
      }
      for (const auto item : orig_masks_.items()) {
        masks.span[item.key] = item.value;
      }
      masks.finish();
      bke::pbvh::update_mask_mesh(mesh, node_mask, pbvh);
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *ob.runtime->sculpt_session->subdiv_ccg;
      if (subdiv_ccg.masks.is_empty()) {
        /* The Multires mask layer went away under the session (an undo step that predates it, or
         * the modifier being re-leveled). The keys describe an array that no longer exists. */
        return;
      }
      for (const auto item : orig_masks_.items()) {
        subdiv_ccg.masks[item.key] = item.value;
      }
      bke::pbvh::update_mask_grids(subdiv_ccg, node_mask, pbvh);
      break;
    }
    case bke::pbvh::Type::BMesh:
      BLI_assert_unreachable();
      return;
  }

  pbvh.tag_masks_changed(node_mask);
}

void MaskEffect::begin_restamp(const Depsgraph & /*depsgraph*/,
                               Object & /*ob*/,
                               CurvePatchSession & /*patch*/)
{
  /* Nothing to prepare. Relief recomputes vertex normals here because it invalidates them by
   * moving vertices and reads them back as its write direction; a mask changes an attribute only,
   * so the positions and normals the sampler culls against are already pristine. */
}

static float mask_from_sample(const float original, const float value, const bool increase)
{
  /* CurvePatchSampler has already folded StrokeCache::bstrength into `value`. Positive values move
   * toward one using the unmasked part; negative values move toward zero using the mask itself.
   * The direction comes from the brush strength, not from the sampled value, so a texture cannot
   * change the Add/Subtract rule. */
  const float factor = increase ? 1.0f - original : original;
  return std::clamp(original + value * factor, 0.0f, 1.0f);
}

void MaskEffect::apply_pass(const Depsgraph &depsgraph,
                            Object &ob,
                            const Brush &brush,
                            CurvePatchSession &patch,
                            const CurvePatchItem &item)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  const CurvePatchStrokeContext ctx = curve_patch_stroke_context_from_cache(*ss.cache);
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  struct MaskWrite {
    int index;
    float original;
    float value;
    float weight;
  };
  struct LocalData {
    Vector<MaskWrite> writes;
    Vector<int> touched_nodes;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;

  curve_patch_effect_ensure_falloff_curve(brush);
  const float max_radius = curve_patch_max_radius(item.geometry);
  IndexMaskMemory culled_memory;
  const IndexMask node_mask = curve_patch_effect_node_mask(
      depsgraph, ob, brush, item, ctx, pbvh, max_radius, culled_memory);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *id_cast<Mesh *>(ob.data);
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      bke::SpanAttributeWriter<float> masks = attributes.lookup_or_add_for_write_span<float>(
          ".sculpt_mask", bke::AttrDomain::Point);
      if (!masks) {
        return;
      }
      const Span<float3> positions = mesh.vert_positions();
      const Span<float3> normals = mesh.vert_normals();
      /* The sampler's mask input is intentionally empty. MaskEffect applies the original mask from
       * its snapshot itself, which keeps symmetry passes independent from earlier writes. */
      const CurvePatchSourceGeometry source{positions, normals, nullptr};
      const CurvePatchSampler sampler(
          item, patch.doc.texture, ctx, brush, source, Span<float>(), ss.tex_pool_ensure());
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

      node_mask.foreach_index(
          [&](const int node) {
            const int thread_id = BLI_task_parallel_thread_id(nullptr);
            LocalData &local = all_tls.local();
            const int64_t before = local.writes.size();
            for (const int vert : nodes[node].verts()) {
              const std::optional<CurvePatchSample> sample = sampler.sample(vert, thread_id);
              if (!sample) {
                continue;
              }
              const float *original = orig_masks_.lookup_ptr(vert);
              local.writes.append(
                  {vert, original ? *original : masks.span[vert], sample->value, sample->weight});
            }
            if (local.writes.size() > before) {
              local.touched_nodes.append(node);
            }
          },
          exec_mode::grain_size(1));

      IndexMaskMemory tag_memory;
      BitVector<> touched(pbvh.nodes_num(), false);
      for (const LocalData &local : all_tls) {
        for (const int node : local.touched_nodes) {
          touched[node].set();
        }
      }
      const IndexMask tag_mask = IndexMask::from_bits(touched, tag_memory);
      for (const LocalData &local : all_tls) {
        for (const MaskWrite &write : local.writes) {
          orig_masks_.lookup_or_add(write.index, write.original);
          const float blended = curve_patch_blend_across_passes(
              patch.apply, write.index, write.weight, write.value);
          masks.span[write.index] = mask_from_sample(
              write.original, blended, ctx.bstrength > 0.0f);
        }
      }
      tag_mask.foreach_index(
          [&](const int node) { bke::pbvh::node_update_mask_mesh(masks.span, nodes[node]); },
          exec_mode::grain_size(1));
      masks.finish();
      pbvh.tag_masks_changed(tag_mask);
      curve_patch_record_touched_nodes(patch.apply, tag_mask);
      return;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      MutableSpan<float> masks = subdiv_ccg.masks;
      if (masks.is_empty()) {
        /* Unlike a mesh -- where `lookup_or_add_for_write_span()` above grows `.sculpt_mask` on
         * demand -- the Multires mask span cannot be created from here: it mirrors
         * `CD_GRID_PAINT_MASK` on the base mesh, which only `BKE_sculpt_mask_layers_ensure()`
         * builds. Every entry point that can select this effect calls it (the interactive handoff
         * inherits it from `sculpt_brush_stroke_invoke()`, and `curve_patch_apply()` runs it
         * before publishing), so reaching this is a bug elsewhere -- but writing through an empty
         * span would be an out-of-bounds write, so bail instead. */
        BLI_assert_unreachable();
        return;
      }
      const Span<float3> positions = subdiv_ccg.positions;
      const Span<float3> normals = subdiv_ccg.normals;
      /* Grid element indices are not mesh vertex indices. The sampler's normal lookup also guards
       * on `CurvePatchGeometry::surface.ready`, which is false on Grids, but say so locally rather
       * than lean on an invariant established in `curve_patch_session_publish()`. */
      const CurvePatchSourceGeometry source{
          positions, normals, nullptr, /*indices_are_mesh_verts*/ false};
      const CurvePatchSampler sampler(
          item, patch.doc.texture, ctx, brush, source, Span<float>(), ss.tex_pool_ensure());
      const BitVector<> grid_keep = curve_patch_cull_grids(
          item, ctx, pbvh, subdiv_ccg, key, positions, node_mask, max_radius);
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();

      node_mask.foreach_index(
          [&](const int node) {
            const int thread_id = BLI_task_parallel_thread_id(nullptr);
            LocalData &local = all_tls.local();
            const int64_t before = local.writes.size();
            for (const int grid : nodes[node].grids()) {
              if (!grid_keep[grid]) {
                continue;
              }
              for (const int index : bke::ccg::grid_range(key, grid)) {
                const std::optional<CurvePatchSample> sample = sampler.sample(index, thread_id);
                if (!sample) {
                  continue;
                }
                const float *original = orig_masks_.lookup_ptr(index);
                local.writes.append(
                    {index, original ? *original : masks[index], sample->value, sample->weight});
              }
            }
            if (local.writes.size() > before) {
              local.touched_nodes.append(node);
            }
          },
          exec_mode::grain_size(1));

      IndexMaskMemory tag_memory;
      BitVector<> touched(pbvh.nodes_num(), false);
      for (const LocalData &local : all_tls) {
        for (const int node : local.touched_nodes) {
          touched[node].set();
        }
      }
      const IndexMask tag_mask = IndexMask::from_bits(touched, tag_memory);
      for (const LocalData &local : all_tls) {
        for (const MaskWrite &write : local.writes) {
          orig_masks_.lookup_or_add(write.index, write.original);
          const float blended = curve_patch_blend_across_passes(
              patch.apply, write.index, write.weight, write.value);
          masks[write.index] = mask_from_sample(write.original, blended, ctx.bstrength > 0.0f);
        }
      }
      tag_mask.foreach_index(
          [&](const int node) { bke::pbvh::node_update_mask_grids(key, masks, nodes[node]); },
          exec_mode::grain_size(1));
      pbvh.tag_masks_changed(tag_mask);
      curve_patch_record_touched_nodes(patch.apply, tag_mask);
      return;
    }
    case bke::pbvh::Type::BMesh:
      BLI_assert_unreachable();
      return;
  }
}

void MaskEffect::end_restamp(Object & /*ob*/, CurvePatchSession & /*patch*/)
{
  /* Nothing to finish. There is no smoothing counterpart -- that would be the Mask Smooth tool,
   * which `bke::brush::supports_curve_patch()` deliberately keeps out of Curve Patch for want of a
   * neighborhood-aware pass -- and the viewport flush is issued by the session, from
   * #update_type. */
}

void MaskEffect::commit(const Scene &scene,
                        const Depsgraph &depsgraph,
                        Object &ob,
                        const CurvePatchSession &patch)
{
  if (orig_masks_.is_empty()) {
    return;
  }

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  IndexMaskMemory memory;
  const IndexMask node_mask = IndexMask::from_bits(patch.apply.all_touched_nodes, memory);

  /* Flattened once, up front. `undo::push_nodes()` stores whatever the target currently holds, and
   * at commit time that is the PAINTED state, so the originals have to be swapped in across the
   * push and swapped back out after it. Materializing the keys and values instead of walking
   * `orig_masks_` twice is what removes the unstated "both passes see the same `Map` iteration
   * order" contract, and it lets both pbvh types share one primitive -- the same shape
   * `ColorEffect::commit()` gets for free from `color::swap_gathered_colors()`. */
  Array<int> indices(orig_masks_.size());
  Array<float> values(orig_masks_.size());
  int i = 0;
  for (const auto item : orig_masks_.items()) {
    indices[i] = item.key;
    values[i] = item.value;
    i++;
  }
  const auto swap_masks = [&](const MutableSpan<float> masks) {
    for (const int j : indices.index_range()) {
      std::swap(masks[indices[j]], values[j]);
    }
  };

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *id_cast<Mesh *>(ob.data);
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      bke::SpanAttributeWriter<float> masks = attributes.lookup_for_write_span<float>(
          ".sculpt_mask");
      if (!masks) {
        return;
      }
      swap_masks(masks.span);
      undo::push_begin_ex(scene, ob, "Curve Patch Mask");
      undo::push_nodes(depsgraph, ob, node_mask, undo::Type::Mask);
      swap_masks(masks.span);
      masks.finish();
      break;
    }
    case bke::pbvh::Type::Grids: {
      const MutableSpan<float> masks = ob.runtime->sculpt_session->subdiv_ccg->masks;
      if (masks.is_empty()) {
        return;
      }
      swap_masks(masks);
      undo::push_begin_ex(scene, ob, "Curve Patch Mask");
      undo::push_nodes(depsgraph, ob, node_mask, undo::Type::Mask);
      swap_masks(masks);
      break;
    }
    case bke::pbvh::Type::BMesh:
      BLI_assert_unreachable();
      return;
  }

  undo::push_end_ex(ob, false);
}

}  // namespace

std::unique_ptr<CurvePatchEffect> curve_patch_effect_mask_create()
{
  return std::make_unique<MaskEffect>();
}

}  // namespace blender::ed::sculpt_paint
