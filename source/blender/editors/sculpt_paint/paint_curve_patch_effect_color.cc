/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Vertex-color target for a Curve Patch session: mixes the brush color into the active color
 * attribute by the magnitude `CurvePatchSampler` reports, and owns everything needed to take that
 * back -- the snapshot, the restore, and the undo step.
 *
 * Mirrors `paint_curve_patch_effect_relief.cc` in shape. The two differ only in what they
 * snapshot and write; the sampler, the culls, the two-phase skeleton and the cross-pass blend are
 * shared verbatim.
 */

#include <optional>
#include <string>

#include "paint_curve_patch_effect.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_types.hh"

#include "BLI_array.hh"
#include "BLI_bit_vector.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_execution_mode.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "paint_curve_patch_cache.hh"
#include "paint_curve_patch_sampler.hh"
#include "paint_intern.hh"

#include "mesh/mesh_brush_common.hh"
#include "mesh/sculpt_color.hh"
#include "mesh/sculpt_intern.hh"
#include "mesh/sculpt_undo.hh"

namespace blender::ed::sculpt_paint {

namespace {

/** Calls `fn` with every color-attribute element belonging to `vert`: the vertex itself on
 * `AttrDomain::Point`, or each of its corners on `AttrDomain::Corner`. Mirrors the traversal
 * `color::color_vert_set()` performs (`mesh/sculpt_paint_color.cc:112-137`), so the snapshot keys
 * and the written elements are guaranteed to be the same set. */
template<typename Fn>
void foreach_vert_domain_element(const Mesh &mesh,
                                 const bke::AttrDomain domain,
                                 const int vert,
                                 Fn &&fn)
{
  if (domain == bke::AttrDomain::Point) {
    fn(vert);
    return;
  }
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  for (const int face : vert_to_face_map[vert]) {
    fn(bke::mesh::face_find_corner_from_vert(faces[face], corner_verts, vert));
  }
}

class ColorEffect : public CurvePatchEffect {
 public:
  ColorEffect(std::string attribute_name, bke::AttrDomain domain);

  int64_t element_num(Object &ob) const override;
  void restore(Object &ob, const CurvePatchCache &patch) override;
  void begin_restamp(const Depsgraph &depsgraph, Object &ob, CurvePatchCache &patch) override;
  void apply_pass(const Depsgraph &depsgraph,
                  Object &ob,
                  const Brush &brush,
                  CurvePatchCache &patch) override;
  void end_restamp(bContext &C, Object &ob, CurvePatchCache &patch) override;
  void commit(bContext &C, Object &ob, const CurvePatchCache &patch) override;
  int64_t snapshot_size() const override;

 private:
  /** True when the mesh still carries the same color attribute this session started on.
   * `CurvePatchCache::element_num` cannot catch a same-size replacement, and the modal passes
   * events through, so the attribute can be switched from a panel mid-session. */
  bool attribute_matches(const Mesh &mesh) const;

  /** Name and domain of the color attribute frozen at session start. */
  std::string attribute_name_;
  bke::AttrDomain domain_;

  /** Lazily-grown snapshot of original (pre-patch) colors, keyed by DOMAIN ELEMENT index -- a
   * vertex index on `AttrDomain::Point`, a corner index on `AttrDomain::Corner`. Keyed by domain
   * rather than by vertex because `color::color_vert_get()` averages a vertex' corner colors and
   * `color::color_vert_set()` broadcasts one value back to all of them: a per-vertex snapshot
   * would flatten every hard color border the first time the patch touched it, and restore runs
   * on every curve edit.
   *
   * Unlike `ReliefEffect::orig_positions_` this is NOT read by the sampler -- color does not move
   * geometry, so the sampler's pre-patch positions are simply the live ones. It is pure undo and
   * restore bookkeeping. */
  Map<int, float4> orig_colors_;
};

ColorEffect::ColorEffect(std::string attribute_name, const bke::AttrDomain domain)
    : attribute_name_(std::move(attribute_name)), domain_(domain)
{
}

bool ColorEffect::attribute_matches(const Mesh &mesh) const
{
  if (mesh.active_color_attribute != attribute_name_) {
    return false;
  }
  /* `lookup_meta_data()` returns `std::optional<AttributeMetaData>` (`BKE_attribute.hh:549`),
   * not a pointer. */
  const std::optional<bke::AttributeMetaData> meta = mesh.attributes().lookup_meta_data(
      attribute_name_);
  return meta.has_value() && meta->domain == domain_;
}

/** Size of the domain the snapshot keys index into. Point-domain keys are vertex indices,
 * Corner-domain keys are corner indices, and the two counts move independently: Triangulate and
 * Poke change `corners_num` while leaving `verts_num` untouched, so reporting the wrong one would
 * let restore write through stale indices. */
int64_t ColorEffect::element_num(Object &ob) const
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    return 0;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  return domain_ == bke::AttrDomain::Corner ? mesh.corners_num : mesh.verts_num;
}

int64_t ColorEffect::snapshot_size() const
{
  return orig_colors_.size();
}

void ColorEffect::restore(Object &ob, const CurvePatchCache &patch)
{
  if (this->element_num(ob) != patch.element_num) {
    /* See `CurvePatchCache::element_num`: writing the snapshot back would corrupt an unrelated
     * mesh. This also makes the ordinary cancel path safe for an invalidated patch. */
    return;
  }
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  if (!this->attribute_matches(mesh)) {
    /* The active color attribute was swapped mid-session. The keys describe a different array
     * now, so there is nothing safe to restore. */
    return;
  }
  bke::GSpanAttributeWriter colors = color::active_color_attribute_for_write(mesh);
  if (!colors) {
    return;
  }

  Vector<int> indices;
  Array<float4> values(orig_colors_.size());
  indices.reserve(orig_colors_.size());
  int i = 0;
  for (const auto item : orig_colors_.items()) {
    indices.append(item.key);
    values[i++] = item.value;
  }
  /* Swap rather than one-way write: `swap_gathered_colors()` leaves `values` holding what was in
   * the attribute, which this function discards -- but the same call is what `commit()` relies on
   * for symmetric undo/redo, so both paths use one primitive. */
  color::swap_gathered_colors(indices, colors.span, values);
  colors.finish();

  IndexMaskMemory memory;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  pbvh.tag_attribute_changed(IndexMask::from_bits(patch.last_restamp_nodes, memory),
                             mesh.active_color_attribute);
}

void ColorEffect::begin_restamp(const Depsgraph & /*depsgraph*/,
                                Object & /*ob*/,
                                CurvePatchCache & /*patch*/)
{
  /* Relief recomputes vertex normals here because it invalidates them by moving vertices, and
   * because it reads them both as its write direction and as the sampler's orientation cull.
   * Color does neither: it never moves geometry, and on Mesh the cull reads the pristine
   * `CurvePatchCache::surface` snapshot. Nothing to prepare. */
}

void ColorEffect::apply_pass(const Depsgraph &depsgraph,
                             Object &ob,
                             const Brush &brush,
                             CurvePatchCache &patch)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;

  IndexMaskMemory memory;
  const brushes::CursorSampleResult cursor_sample_result = calc_brush_node_mask(
      depsgraph, ob, brush, memory);
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  if (!this->attribute_matches(mesh)) {
    return;
  }
  bke::GSpanAttributeWriter colors = color::active_color_attribute_for_write(mesh);
  if (!colors) {
    return;
  }
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> normals = mesh.vert_normals();
  const MeshAttributeData attribute_data(mesh);
  const Span<float> mask = attribute_data.mask;

  /* `BKE_brush_curve_strength()` below reads `brush.curve_distance_falloff`'s lookup table for the
   * CUSTOM preset; initialize it ONCE here so the parallel PHASE 1 loop only ever reads an
   * already-built table (a lazy init inside a worker thread would race). */
  if (brush.curve_distance_falloff) {
    BKE_curvemapping_init(brush.curve_distance_falloff);
  }

  /* Source geometry has no snapshot override: color never moves geometry, so the live positions
   * are already pristine and `CurvePatchSampler` can read them directly. */
  const CurvePatchSourceGeometry source{positions, normals, nullptr};
  const CurvePatchSampler sampler(patch, cache, brush, source, mask, ss.tex_pool);

  const float max_radius = curve_patch_max_radius(patch);

  IndexMaskMemory culled_memory;
  const IndexMask node_mask = curve_patch_cull_nodes(
      patch, cache, pbvh, cursor_sample_result.node_mask, max_radius, culled_memory);

  /* PHASE 1 (parallel, read-only): each pbvh node is processed on a worker thread; surviving
   * vertices are expanded into their domain elements and the pre-patch color of each is gathered,
   * together with the per-vertex sample, into a thread-local buffer. No color or snapshot-map
   * writes happen here, so the reads inside `compute_vertex()` are race-free across threads
   * (texture sampling uses the per-thread pool slot `thread_id`). */
  struct ColorWrite {
    int idx;        /* Domain element index. */
    float4 orig;    /* Pre-patch color of that element. */
    float4 tex_color; /* Brush-texture RGBA at this sample (`{1,1,1,1}` when no texture). */
    float value;    /* Mix magnitude from the sampler. */
    float weight;   /* Cross-pass claim weight. */
  };
  struct LocalData {
    Vector<ColorWrite> writes;
    Vector<int> touched_nodes;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  {
    const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
    node_mask.foreach_index(
        [&](const int i) {
          const int thread_id = BLI_task_parallel_thread_id(nullptr);
          LocalData &local = all_tls.local();
          const int64_t before = local.writes.size();
          for (const int vert : nodes[i].verts()) {
            const std::optional<CurvePatchSample> sample = sampler.sample(vert, thread_id);
            if (!sample) {
              continue;
            }
            foreach_vert_domain_element(mesh, domain_, vert, [&](const int elem) {
              const float4 *orig_ptr = orig_colors_.lookup_ptr(elem);
              float4 orig;
              if (orig_ptr) {
                orig = *orig_ptr;
              }
              else {
                /* One-element spans over stack variables: `gather_colors()` is the existing
                 * generic reader (it handles both `MPropCol` and `MLoopCol` including the sRGB
                 * conversion), and this form avoids allocating per vertex. */
                color::gather_colors(
                    colors.span, Span<int>(&elem, 1), MutableSpan<float4>(&orig, 1));
              }
              local.writes.append({elem, orig, sample->tex_color, sample->value, sample->weight});
            });
          }
          if (local.writes.size() > before) {
            local.touched_nodes.append(i);
          }
        },
        exec_mode::grain_size(1));
  }

  /* Scope the attribute-change tag to the nodes that ACTUALLY received a write, not the whole
   * encompassing-sphere query. On a dense mesh the query is ~20-30x larger than the thin strip the
   * color lands on, and tagging all of it would force the draw path to refresh nodes the patch
   * never touched. */
  IndexMaskMemory tag_memory;
  BitVector<> touched(pbvh.nodes_num(), false);
  for (const LocalData &local : all_tls) {
    for (const int node : local.touched_nodes) {
      touched[node].set();
    }
  }
  const IndexMask tag_mask = IndexMask::from_bits(touched, tag_memory);

  /* PHASE 2 (serial): the sole writer of the color attribute and of `orig_colors_`. */
  const Paint &paint = *cache.paint;
  const float3 brush_color = BKE_brush_color_get(&paint, &brush);
  /* The RGB the patch paints is ALWAYS the brush's primary color. A brush texture contributes only
   * its intensity -- already folded into `CurvePatchSample::value` by the sampler -- and its alpha,
   * which attenuates the mix below (an image texture with transparent regions presses through
   * weaker). `CurvePatchSample::tex_color`'s RGB is deliberately left unread until the color path
   * grows real RGBA-texture support; `has_texture` exists only to tell a meaningful alpha from the
   * `{1,1,1,1}` the sampler leaves behind when no texture is assigned, since the texture field
   * alone cannot tell "color sampled" from "value sampled" (`paint_get_tex_pixel`'s return value
   * was verified not to encode that). */
  const bool has_texture = brush.mtex.tex != nullptr;

  for (LocalData &local : all_tls) {
    for (const ColorWrite &write : local.writes) {
      orig_colors_.lookup_or_add(write.idx, write.orig);

      /* Blend this pass's contribution with any earlier symmetry pass of this restamp that also
       * claimed `write.idx` -- see `ReliefEffect::apply_pass()` for the rationale (a patch
       * straddling a mirror/radial symmetry plane can have both the direct and the mirrored pass
       * land on the same real element). */
      float2 &accum = patch.pass_weight_accum.lookup_or_add(write.idx, float2(0.0f, 0.0f));
      accum.x += write.weight;
      accum.y += write.weight * write.value;
      const float blended = accum.y / accum.x;

      /* Relief has no `abs()` anywhere -- a negative `bstrength` (Subtract direction, or a CUSTOM
       * falloff curve dipping below zero) legitimately carves inward. A negative mix factor has no
       * such meaning, so clamp instead of preserving the sign. This is also why the brush's
       * Add/Subtract toggle has no visible effect on a color patch. The texture's own alpha further
       * attenuates the factor, so a partially-transparent texel paints at partial strength -- the
       * same outcome a per-dab brush gets by multiplying the dab's alpha by the texture's. */
      const float source_alpha = has_texture ? write.tex_color.w : 1.0f;
      const float factor = std::clamp(blended, 0.0f, 1.0f) * source_alpha;

      /* The original alpha is carried through untouched, textured or not: `BKE_brush_color_get()`
       * returns a `float3`, so writing an alpha would invent data the brush never specified (the
       * Stage 3 invariant). */
      float4 mixed(math::interpolate(float3(write.orig), brush_color, factor), write.orig.w);
      /* `swap_gathered_colors()` is the generic writer as well as the reader: it exchanges the
       * element with `mixed`, leaving the previous value in `mixed`, which this path discards.
       * Using it keeps the byte-color conversion symmetric with the read in PHASE 1. */
      color::swap_gathered_colors(
          Span<int>(&write.idx, 1), colors.span, MutableSpan<float4>(&mixed, 1));
    }
  }
  colors.finish();

  /* Tag only the nodes that actually received a color write, so the draw path refreshes exactly
   * that thin strip and not the whole encompassing-sphere query. Mirrors the position tag
   * `ReliefEffect::apply_pass()` does. */
  pbvh.tag_attribute_changed(tag_mask, mesh.active_color_attribute);

  /* Remember the nodes this pass touched (accumulated across symmetry passes) so the NEXT
   * restamp's `restore()` can tag exactly these for re-draw. `curve_patch_restore_and_restamp()`
   * sizes and clears this bit set before the first pass runs. */
  tag_mask.set_bits(patch.last_restamp_nodes);

  /* The same bits also accumulate into the patch's lifetime union, which is never cleared and is
   * what the commit-time undo step is pushed over -- see `CurvePatchCache::all_touched_nodes`. */
  tag_mask.set_bits(patch.all_touched_nodes);
}

void ColorEffect::end_restamp(bContext & /*C*/, Object &ob, CurvePatchCache &patch)
{
  /* No smoothing counterpart: `ReliefEffect::smooth_relief()` averages displacement VECTORS, which
   * has no color analogue worth inventing. */
  flush_update_step(patch.view_context, ob, UpdateType::Color);
}

void ColorEffect::commit(bContext &C, Object &ob, const CurvePatchCache &patch)
{
  if (this->element_num(ob) != patch.element_num) {
    /* An undo step built from a stale snapshot would be worse than no step at all. */
    return;
  }
  if (orig_colors_.is_empty()) {
    return;
  }
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  if (!this->attribute_matches(mesh)) {
    return;
  }
  bke::GSpanAttributeWriter colors = color::active_color_attribute_for_write(mesh);
  if (!colors) {
    return;
  }

  const Scene &scene = *CTX_data_scene(&C);
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(&C);

  Vector<int> indices;
  Array<float4> values(orig_colors_.size());
  indices.reserve(orig_colors_.size());
  int i = 0;
  for (const auto item : orig_colors_.items()) {
    indices.append(item.key);
    values[i++] = item.value;
  }

  /* `undo::push_nodes()` stores whatever the attribute currently holds, and at commit time that is
   * the painted state. Swap the originals in, push, swap the painted colors back -- the same trick
   * `ReliefEffect::push_position_step()` uses, except `swap_gathered_colors()` IS a swap, so the
   * round trip is two calls with no temporary of our own. */
  IndexMaskMemory memory;
  color::swap_gathered_colors(indices, colors.span, values);
  undo::push_begin_ex(scene, ob, "Curve Patch Color");
  undo::push_nodes(
      depsgraph, ob, IndexMask::from_bits(patch.all_touched_nodes, memory), undo::Type::Color);
  color::swap_gathered_colors(indices, colors.span, values);
  colors.finish();

  /* `false`, never forced: unlike relief there is no face-set step to follow, so the step must stay
   * parked in `ustack->step_init` for `wm_operator_finished()`. Forcing it here would cost the user
   * one dead Ctrl+Z before the color is undone. */
  undo::push_end_ex(ob, false);
}

}  // namespace

std::unique_ptr<CurvePatchEffect> curve_patch_effect_color_create(const Object &ob)
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    /* Multires has no color attributes. */
    return nullptr;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const StringRef name = mesh.active_color_attribute;
  if (name.is_empty()) {
    return nullptr;
  }
  const std::optional<bke::AttributeMetaData> meta = mesh.attributes().lookup_meta_data(name);
  if (!meta || !bke::mesh::is_color_attribute({meta->domain, meta->data_type})) {
    return nullptr;
  }
  return std::make_unique<ColorEffect>(name, meta->domain);
}

}  // namespace blender::ed::sculpt_paint
