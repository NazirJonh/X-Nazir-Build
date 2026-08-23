/* SPDX-FileCopyrightText: 2026 Nazir Galimov
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
#include <utility>

#include "paint_curve_patch_effect.hh"

#include "DNA_brush_types.h"
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
#include "BLI_assert.h"
#include "BLI_bit_vector.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_execution_mode.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "paint_curve_patch_effect_common.hh"
#include "paint_intern.hh"

#include "mesh/mesh_brush_common.hh"
#include "mesh/paint_material_blend.hh"
#include "mesh/paint_vertex_channel_mask.hh"
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

/** Which color array the effect writes, and by which rules.
 *
 * `ActiveAttribute` is the ordinary Color Attribute canvas: the mesh's active color attribute,
 * the brush color, a plain Mix, and a #undo::Type::Color step. `MaterialChannel` is Poly Paint's
 * Base Color: an attribute addressed BY NAME (the active-attribute pointer may point elsewhere
 * entirely), the channel's own color and blend mode, and a #undo::Type::Material step. */
enum class ColorTarget : int8_t {
  ActiveAttribute,
  MaterialChannel,
};

/** One scalar material channel a Poly Paint patch writes.
 *
 * A stroke leans on its undo step for the pre-stroke value (#orig_material_scalar_data_lookup_mesh);
 * a patch cannot, because it is re-stamped from scratch on every curve edit and has to be able
 * to put the surface back first. So each channel carries its own snapshot, exactly as the color
 * target does. */
struct ScalarTarget {
  std::string name;
  eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_METALLIC;
  /** Original (pre-patch) values keyed by POINT index -- scalar channels are always point
   * attributes, so unlike the color snapshot there is no domain expansion here. */
  Map<int, float> orig_values;
};

/** Exchanges \a snapshot's values with what \a values currently holds. Calling it twice is the
 * identity, which is what lets `commit()` show undo the originals and then put the painted
 * result back -- the same trick the color path gets from `swap_gathered_colors()`. */
static void swap_scalar_snapshot(Map<int, float> &snapshot, MutableSpan<float> values)
{
  for (const int key : snapshot.keys()) {
    if (key < 0 || key >= values.size()) {
      continue;
    }
    float *stored = snapshot.lookup_ptr(key);
    std::swap(values[key], *stored);
  }
}

class ColorEffect : public CurvePatchEffect {
 public:
  ColorEffect(std::string attribute_name,
              bke::AttrDomain domain,
              ColorTarget target,
              PaintModeSettings *paint_mode_settings,
              Vector<ScalarTarget> scalar_targets);

  int64_t element_num(Object &ob) const override;
  void restore(Object &ob, const CurvePatchSession &patch) override;
  void begin_restamp(const Depsgraph &depsgraph, Object &ob, CurvePatchSession &patch) override;
  void apply_pass(const Depsgraph &depsgraph,
                  Object &ob,
                  const Brush &brush,
                  CurvePatchSession &patch,
                  const CurvePatchItem &item) override;
  UpdateType update_type() const override
  {
    return UpdateType::Color;
  }
  void end_restamp(Object &ob, CurvePatchSession &patch) override;
  void commit(const Scene &scene,
              const Depsgraph &depsgraph,
              Object &ob,
              const CurvePatchSession &patch) override;
  int64_t snapshot_size() const override;

 private:
  /** True when the mesh still carries the same color attribute this session started on.
   * `CurvePatchApplyState::element_num` cannot catch a same-size replacement, and the modal passes
   * events through, so the attribute can be switched from a panel mid-session. */
  bool attribute_matches(const Mesh &mesh) const;

  /** The attribute this effect writes, by name -- see #color_writer. */
  bke::GSpanAttributeWriter color_writer(Mesh &mesh) const;

  /** Name and domain of the color attribute frozen at session start. */
  std::string attribute_name_;
  bke::AttrDomain domain_;
  ColorTarget target_;
  /** False when Poly Paint has Base Color disabled and only scalar channels are painted; then
   * #attribute_name_ is empty and no color array is touched at all. */
  bool has_color_target_;
  /** Empty for every canvas except Poly Paint. */
  Vector<ScalarTarget> scalar_targets_;
  /** Raw pointer, not owned: `scene->toolsettings->paint_mode`, stable for the scene's
   * lifetime. Null for #ColorTarget::ActiveAttribute, which needs none of it. */
  PaintModeSettings *paint_mode_settings_;

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

ColorEffect::ColorEffect(std::string attribute_name,
                         const bke::AttrDomain domain,
                         const ColorTarget target,
                         PaintModeSettings *paint_mode_settings,
                         Vector<ScalarTarget> scalar_targets)
    : attribute_name_(std::move(attribute_name)),
      domain_(domain),
      target_(target),
      has_color_target_(!attribute_name_.empty()),
      scalar_targets_(std::move(scalar_targets)),
      paint_mode_settings_(paint_mode_settings)
{
  BLI_assert(target_ != ColorTarget::MaterialChannel || paint_mode_settings_ != nullptr);
  BLI_assert(has_color_target_ || !scalar_targets_.is_empty());
  BLI_assert(target_ == ColorTarget::MaterialChannel || scalar_targets_.is_empty());
}

bke::GSpanAttributeWriter ColorEffect::color_writer(Mesh &mesh) const
{
  if (target_ == ColorTarget::ActiveAttribute) {
    return color::active_color_attribute_for_write(mesh);
  }
  /* A material channel attribute is NOT reached through the active-attribute pointer: Poly
   * Paint writes several attributes at once and none of them need be the active one.
   * #attribute_matches has already established that this name holds a color array. */
  return mesh.attributes_for_write().lookup_for_write_span(attribute_name_);
}

bool ColorEffect::attribute_matches(const Mesh &mesh) const
{
  /* Only the Color Attribute canvas follows the active-attribute pointer; a material channel is
   * bound to its own name and is unaffected by the user making some other attribute active. */
  if (target_ == ColorTarget::ActiveAttribute && mesh.active_color_attribute != attribute_name_)
  {
    return false;
  }
  /* `lookup_meta_data()` returns `std::optional<AttributeMetaData>` (`BKE_attribute.hh:549`),
   * not a pointer. */
  const std::optional<bke::AttributeMetaData> meta = mesh.attributes().lookup_meta_data(
      attribute_name_);
  if (!meta.has_value() || meta->domain != domain_) {
    return false;
  }
  /* The active-attribute path gets this guarantee from #active_color_attribute_for_write; a
   * named lookup does not, and writing float4 through, say, a float attribute of the same
   * element count would corrupt it. */
  return bke::mesh::is_color_attribute({meta->domain, meta->data_type});
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
  /* With no color target the only arrays this effect writes are point attributes, so the count
   * that has to stay stable is the vertex count. */
  if (!has_color_target_) {
    return mesh.verts_num;
  }
  return domain_ == bke::AttrDomain::Corner ? mesh.corners_num : mesh.verts_num;
}

int64_t ColorEffect::snapshot_size() const
{
  int64_t size = orig_colors_.size();
  for (const ScalarTarget &target : scalar_targets_) {
    size += target.orig_values.size();
  }
  return size;
}

void ColorEffect::restore(Object &ob, const CurvePatchSession &patch)
{
  /* No `element_num` check of its own -- `curve_patch_restore_only()` performs it for every
   * caller. The attribute check below is NOT redundant with it: a same-size replacement changes no
   * count. */
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  if (has_color_target_ && !this->attribute_matches(mesh)) {
    /* The color attribute was swapped mid-session. Its keys describe a different array now, so
     * there is nothing safe to restore THROUGH IT -- but the scalar channels below are keyed by
     * their own names and are unaffected, so this is no longer a whole-function refusal. */
    has_color_target_ = false;
  }
  IndexMaskMemory memory;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  const IndexMask restamp_mask = IndexMask::from_bits(patch.apply.last_restamp_nodes, memory);

  if (has_color_target_) {
    bke::GSpanAttributeWriter colors = this->color_writer(mesh);
    if (colors) {
      Vector<int> indices;
      Array<float4> values(orig_colors_.size());
      indices.reserve(orig_colors_.size());
      int i = 0;
      for (const auto item : orig_colors_.items()) {
        indices.append(item.key);
        values[i++] = item.value;
      }
      /* Swap rather than one-way write: `swap_gathered_colors()` leaves `values` holding what
       * was in the attribute, which this function discards -- but the same call is what
       * `commit()` relies on for symmetric undo/redo, so both paths use one primitive. */
      color::swap_gathered_colors(indices, colors.span, values);
      colors.finish();
      pbvh.tag_attribute_changed(restamp_mask, attribute_name_);
    }
  }

  /* Poly Paint's scalar channels, each with its own snapshot. A channel whose attribute has
   * since been deleted or retyped is skipped rather than refused: the other channels' pixels
   * still have to come back, or the next re-stamp would compound on top of them. */
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  for (ScalarTarget &target : scalar_targets_) {
    if (target.orig_values.is_empty()) {
      continue;
    }
    bke::SpanAttributeWriter<float> attribute = attributes.lookup_for_write_span<float>(
        target.name);
    if (!attribute) {
      continue;
    }
    swap_scalar_snapshot(target.orig_values, attribute.span);
    attribute.finish();
    pbvh.tag_attribute_changed(restamp_mask, target.name);
  }
}

void ColorEffect::begin_restamp(const Depsgraph & /*depsgraph*/,
                                Object & /*ob*/,
                                CurvePatchSession & /*patch*/)
{
  /* Relief recomputes vertex normals here because it invalidates them by moving vertices, and
   * because it reads them both as its write direction and as the sampler's orientation cull.
   * Color does neither: it never moves geometry, and on Mesh the cull reads the pristine
   * `CurvePatchGeometry::surface` snapshot. Nothing to prepare. */
}

void ColorEffect::apply_pass(const Depsgraph &depsgraph,
                             Object &ob,
                             const Brush &brush,
                             CurvePatchSession &patch,
                             const CurvePatchItem &item)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;
  /* Snapshotted per PASS, not per re-stamp: `do_symmetrical_brush_actions()` rewrites every field
   * of it between the passes it drives. */
  const CurvePatchStrokeContext ctx = curve_patch_stroke_context_from_cache(cache);

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  if (has_color_target_ && !this->attribute_matches(mesh)) {
    return;
  }
  bke::GSpanAttributeWriter colors;
  if (has_color_target_) {
    colors = this->color_writer(mesh);
    if (!colors) {
      return;
    }
  }
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> normals = mesh.vert_normals();
  const MeshAttributeData attribute_data(mesh);
  const Span<float> mask = attribute_data.mask;

  curve_patch_effect_ensure_falloff_curve(brush);

  /* Source geometry has no snapshot override: color never moves geometry, so the live positions
   * are already pristine and `CurvePatchSampler` can read them directly. */
  const CurvePatchSourceGeometry source{positions, normals, nullptr};
  const CurvePatchSampler sampler(
      item, patch.doc.texture, ctx, brush, source, mask, ss.tex_pool_ensure());

  const float max_radius = curve_patch_max_radius(item.geometry);

  IndexMaskMemory culled_memory;
  const IndexMask node_mask = curve_patch_effect_node_mask(
      depsgraph, ob, brush, item, ctx, pbvh, max_radius, culled_memory);

  /* PHASE 1 (parallel, read-only): each pbvh node is processed on a worker thread; surviving
   * vertices are expanded into their domain elements and the pre-patch color of each is gathered,
   * together with the per-vertex sample, into a thread-local buffer. No color or snapshot-map
   * writes happen here, so the reads inside `compute_vertex()` are race-free across threads
   * (texture sampling uses the per-thread pool slot `thread_id`). */
  struct ColorWrite {
    int idx;          /* Domain element index. */
    float4 orig;      /* Pre-patch color of that element. */
    float4 tex_color; /* Brush-texture RGBA at this sample (`{1,1,1,1}` when no texture). */
    bool tex_valid;   /* Whether that RGBA is a real sample -- #CurvePatchSample::tex_valid. */
    float value;      /* Mix magnitude from the sampler. */
    float weight;     /* Cross-pass claim weight. */
  };
  /* Scalar channels are point attributes, so they need the per-VERTEX sample rather than the
   * per-domain-element expansion the color write does. Recorded once and replayed for every
   * channel in PHASE 2: the patch's coverage of a vertex is a property of the geometry, not of
   * which channel is reading it. */
  struct ScalarWrite {
    int vert;
    float4 tex_color;
    bool tex_valid;
    float value;
    float weight;
  };
  struct LocalData {
    Vector<ColorWrite> writes;
    Vector<ScalarWrite> scalar_writes;
    Vector<int> touched_nodes;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  {
    const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
    node_mask.foreach_index(
        [&](const int i) {
          const int thread_id = BLI_task_parallel_thread_id(nullptr);
          LocalData &local = all_tls.local();
          const int64_t before = local.writes.size() + local.scalar_writes.size();
          for (const int vert : nodes[i].verts()) {
            const std::optional<CurvePatchSample> sample = sampler.sample(vert, thread_id);
            if (!sample) {
              continue;
            }
            if (!scalar_targets_.is_empty()) {
              local.scalar_writes.append(
                  {vert, sample->tex_color, sample->tex_valid, sample->value, sample->weight});
            }
            if (!has_color_target_) {
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
              local.writes.append(
                  {elem, orig, sample->tex_color, sample->tex_valid, sample->value,
                   sample->weight});
            });
          }
          if (local.writes.size() + local.scalar_writes.size() > before) {
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
  /* Poly Paint's Base Color is a material channel: it carries its own color and its own blend
   * mode, resolved exactly as `paint_color_channel()` resolves them
   * (`mesh/sculpt_paint_material.cc`) so the stroke engine and the patch cannot drift on what
   * the channel means. The Color Attribute canvas keeps the brush color and a plain Mix. */
  const bool material_channel = target_ == ColorTarget::MaterialChannel &&
                                brush.material_paint != nullptr;
  if (target_ == ColorTarget::MaterialChannel && !material_channel) {
    /* The brush lost its material paint settings mid-session; there is no channel color to
     * paint with, and falling back to the brush color would write a value the channel never
     * specified. */
    colors.finish();
    return;
  }
  if (material_channel && !BKE_paint_material_channel_writes_to_target(
                              *brush.material_paint,
                              *paint_mode_settings_,
                              paint.visible_material_channels,
                              PAINT_MATERIAL_CHANNEL_BASE_COLOR))
  {
    /* Base Color was switched off for this brush after the session started. Writing anyway
     * would put the channel back on a canvas the user has since disabled. */
    colors.finish();
    return;
  }
  const float3 brush_color = material_channel ?
                                 BKE_paint_material_channel_color_get(
                                     *brush.material_paint,
                                     paint,
                                     brush,
                                     PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                     cache.toggle_settings.invert) :
                                 BKE_brush_color_get(&paint, &brush);
  /* The brush's per-channel R/G/B/A toggles. `do_paint_brush_task()` applies them on the Color
   * Attribute canvas (`mesh/sculpt_paint_color.cc`), so a patch on that canvas has to as well or
   * the same brush would honor the toggles for a stroke and ignore them for a patch.
   *
   * Poly Paint is deliberately excluded: `do_paint_material_brush()` does not consult them
   * either -- a material channel decides what it writes through its own enable flag, not through
   * the vertex-paint channel mask. */
  const VPaintChannelMask channel_mask =
      material_channel ? VPaintChannelMask{true, true, true, true} :
                         VPaintChannelMask::from_flag(brush.vertex_paint_channel_flag);
  const IMB_BlendMode blend_mode = material_channel ?
                                       IMB_BlendMode(BKE_paint_material_channel_blend_mode(
                                           *brush.material_paint,
                                           PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                           cache.toggle_settings.invert)) :
                                       IMB_BLEND_MIX;
  /* Whichever of the two colors above applies, a brush texture contributes only
   * its intensity -- already folded into `CurvePatchSample::value` by the sampler -- and its
   * alpha, which attenuates the mix below (an image texture with transparent regions presses
   * through weaker). `CurvePatchSample::tex_color`'s RGB is deliberately left unread until the
   * color path grows real RGBA-texture support; #CurvePatchSample::tex_valid exists only to tell
   * a meaningful alpha from the `{1,1,1,1}` the sampler leaves behind when no texture was
   * evaluated at that element. */
  /* The Strength slider, applied as a separate factor exactly as the ordinary paint pipeline does
   * -- `brush_strength()` leaves it out of `bstrength` for a Paint brush. See
   * #curve_patch_color_mix_factor. */
  const float strength = BKE_brush_alpha_get(&paint, &brush);

  for (LocalData &local : all_tls) {
    for (const ColorWrite &write : local.writes) {
      BLI_assert(has_color_target_);
      orig_colors_.lookup_or_add(write.idx, write.orig);

      /* TODO(I10): `blended` averages every claiming pass/patch's mix factor; `brush_color` is
       * this call's brush, so a later overlapping patch replaces the RGB. Options on
       * #curve_patch_blend_across_passes. */
      const float blended = curve_patch_blend_across_passes(
          patch.apply, write.idx, write.weight, write.value);
      const float factor = curve_patch_color_mix_factor(
          blended, write.tex_color, write.tex_valid, strength);

      /* The original alpha is carried through untouched, textured or not: `BKE_brush_color_get()`
       * returns a `float3`, so writing an alpha would invent data the brush never specified (the
       * Stage 3 invariant). Mix is from the pre-patch original, not from a previous patch's write.
       */
      /* Base Color and the plain color attribute both write a color, so both take the ribbon
       * texture's RGB when one is assigned. */
      const float3 paint_rgb = curve_patch_paint_color(
          brush_color, write.tex_color, write.tex_valid);
      float4 mixed;
      if (blend_mode == IMB_BLEND_MIX) {
        mixed = float4(math::interpolate(float3(write.orig), paint_rgb, factor),
                       write.orig.w);
      }
      else {
        /* `write.orig` is the frozen pre-patch color and `factor` this restamp's total
         * coverage of the element -- the (pre-stroke value, accumulated coverage) pair
         * #composite_coverage expects. The patch snapshot plays the role
         * #StrokeCache::material_mix_base_color plays for a stroke. Pre-multiplied, as that
         * contract requires. */
        mixed = material::composite_coverage(
            write.orig, float4(paint_rgb * factor, factor), blend_mode);
        mixed.w = write.orig.w;
      }
      /* Restore from the frozen pre-patch color rather than from the live element: a patch is
       * re-stamped from the original on every curve edit, so that IS the value a disabled
       * channel must keep. */
      if (!channel_mask.r) {
        mixed.x = write.orig.x;
      }
      if (!channel_mask.g) {
        mixed.y = write.orig.y;
      }
      if (!channel_mask.b) {
        mixed.z = write.orig.z;
      }
      if (!channel_mask.a) {
        mixed.w = write.orig.w;
      }
      /* `swap_gathered_colors()` is the generic writer as well as the reader: it exchanges the
       * element with `mixed`, leaving the previous value in `mixed`, which this path discards.
       * Using it keeps the byte-color conversion symmetric with the read in PHASE 1. */
      color::swap_gathered_colors(
          Span<int>(&write.idx, 1), colors.span, MutableSpan<float4>(&mixed, 1));
    }
  }
  if (colors) {
    colors.finish();
    /* Tag only the nodes that actually received a color write, so the draw path refreshes
     * exactly that thin strip and not the whole encompassing-sphere query. Mirrors the position
     * tag `ReliefEffect::apply_pass()` does. */
    pbvh.tag_attribute_changed(tag_mask, attribute_name_);
  }

  /* Poly Paint's scalar channels. The geometry work is already done: `blended` is the patch's
   * coverage of the VERTEX, which every channel shares, so it is computed once per vertex and
   * replayed per channel -- the same split the image effect makes across its canvases.
   *
   * The accumulator key is deliberately negative. `pass_weight_accum` is one map for the whole
   * restamp, and the color family above keys it by DOMAIN ELEMENT index; on a Point-domain
   * color attribute those indices are vertex indices too, so a shared key space would make the
   * two families accumulate into each other's entries. Negating keeps them disjoint without
   * packing, and cannot overflow the way a multiplied key would on a dense mesh. */
  if (!scalar_targets_.is_empty()) {
    bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
    for (ScalarTarget &target : scalar_targets_) {
      bke::SpanAttributeWriter<float> attribute = attributes.lookup_for_write_span<float>(
          target.name);
      if (!attribute) {
        /* Deleted or retyped since the session started. */
        continue;
      }
      const float2 range = BKE_paint_material_channel_range(*paint_mode_settings_,
                                                            target.channel);
      /* Erasing pulls the channel back to its neutral value; the blend mode that goes with that
       * is already decided by #BKE_paint_material_channel_blend_mode. Mirrors
       * `do_paint_material_brush()`. */
      const float target_value = cache.toggle_settings.invert ?
                                     BKE_paint_material_channel_default_value(target.channel) :
                                     BKE_paint_material_channel_value(*brush.material_paint,
                                                                      *paint_mode_settings_,
                                                                      target.channel);
      const IMB_BlendMode channel_blend = IMB_BlendMode(BKE_paint_material_channel_blend_mode(
          *brush.material_paint, target.channel, cache.toggle_settings.invert));
      const MutableSpan<float> values = attribute.span;
      for (LocalData &local : all_tls) {
        for (const ScalarWrite &write : local.scalar_writes) {
          if (write.vert < 0 || write.vert >= values.size()) {
            continue;
          }
          const float orig = target.orig_values.lookup_or_add(write.vert, values[write.vert]);
          const float blended = curve_patch_blend_across_passes(
              patch.apply, -(write.vert + 1), write.weight, write.value);
          const float factor = curve_patch_color_mix_factor(
              blended, write.tex_color, write.tex_valid, strength);
          /* Pre-multiplied coverage, exactly what `accumulate_scalar_coverage()` builds up over
           * a stroke's dabs -- here the patch snapshot plus `factor` already are that pair. */
          const float blended_value = material::apply_scalar_blend(
              orig, float2(target_value * factor, factor), channel_blend);
          values[write.vert] = math::clamp(blended_value, range.x, range.y);
        }
      }
      attribute.finish();
      pbvh.tag_attribute_changed(tag_mask, target.name);
    }
  }

  curve_patch_record_touched_nodes(patch.apply, tag_mask);
}

void ColorEffect::end_restamp(Object & /*ob*/, CurvePatchSession & /*patch*/)
{
  /* Nothing to finish. There is no smoothing counterpart -- `ReliefEffect::smooth_relief()`
   * averages displacement VECTORS, which has no color analogue worth inventing -- and the viewport
   * flush is issued by the session now, from #update_type. */
}

void ColorEffect::commit(const Scene &scene,
                         const Depsgraph &depsgraph,
                         Object &ob,
                         const CurvePatchSession &patch)
{
  /* No `element_num` check of its own -- see `ReliefEffect::commit()`. */
  bool any_scalar = false;
  for (const ScalarTarget &target : scalar_targets_) {
    any_scalar |= !target.orig_values.is_empty();
  }
  if (orig_colors_.is_empty() && !any_scalar) {
    return;
  }
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  const bool push_color = has_color_target_ && this->attribute_matches(mesh) &&
                          !orig_colors_.is_empty();
  bke::GSpanAttributeWriter colors;
  if (push_color) {
    colors = this->color_writer(mesh);
  }

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
  const IndexMask undo_mask = IndexMask::from_bits(patch.apply.all_touched_nodes, memory);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  /* Held open across the push: the swap below has to be undone through the SAME spans, and
   * reopening them would re-fetch arrays the push may have reallocated. */
  Vector<bke::SpanAttributeWriter<float>> scalar_writers;
  Vector<StringRef> scalar_names;
  /* Parallel to `scalar_writers`: a target whose attribute is gone contributes no writer, so
   * the two cannot be indexed by the same running number as `scalar_targets_`. */
  Vector<int> scalar_written;
  for (const int target_i : scalar_targets_.index_range()) {
    ScalarTarget &target = scalar_targets_[target_i];
    if (target.orig_values.is_empty()) {
      continue;
    }
    bke::SpanAttributeWriter<float> attribute = attributes.lookup_for_write_span<float>(
        target.name);
    if (!attribute) {
      continue;
    }
    swap_scalar_snapshot(target.orig_values, attribute.span);
    scalar_writers.append(std::move(attribute));
    scalar_names.append(target.name);
    scalar_written.append(target_i);
  }
  if (push_color) {
    color::swap_gathered_colors(indices, colors.span, values);
  }
  if (target_ == ColorTarget::MaterialChannel) {
    /* Poly Paint's step is name-keyed and covers scalar and color channels together, so a
     * Base Color write belongs in a #Type::Material step even though nothing scalar is
     * touched -- the same reasoning `brush_stroke_init()` records for a color-only stroke
     * (`mesh/sculpt.cc`). `created_names` stays empty: the patch never creates an attribute,
     * it refuses to start without one (see #curve_patch_effect_color_create). */
    const StringRef color_name = attribute_name_;
    const Span<StringRef> color_names = push_color ? Span<StringRef>(&color_name, 1) :
                                                     Span<StringRef>();
    undo::push_begin_ex(scene, ob, "Curve Patch Material");
    undo::push_nodes(depsgraph,
                     ob,
                     undo_mask,
                     undo::Type::Material,
                     {scalar_names.as_span(), color_names, {}});
  }
  else {
    undo::push_begin_ex(scene, ob, "Curve Patch Color");
    undo::push_nodes(depsgraph, ob, undo_mask, undo::Type::Color);
  }
  if (push_color) {
    color::swap_gathered_colors(indices, colors.span, values);
    colors.finish();
  }
  for (const int i : scalar_writers.index_range()) {
    swap_scalar_snapshot(scalar_targets_[scalar_written[i]].orig_values,
                         scalar_writers[i].span);
    scalar_writers[i].finish();
  }

  /* `false`, never forced: unlike relief there is no face-set step to follow, so the step must
   * stay parked in `ustack->step_init` for `wm_operator_finished()`. Forcing it here would cost
   * the user one dead Ctrl+Z before the color is undone. */
  undo::push_end_ex(ob, false);
}

}  // namespace

std::unique_ptr<CurvePatchEffect> curve_patch_effect_color_create(
    const Object &ob, PaintModeSettings &paint_mode_settings)
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    /* Multires evaluates into grids and Dyntopo into a BMesh; neither carries the point/corner
     * attributes this effect writes. Same restriction as #material::paint_supported_on_object.
     */
    return nullptr;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const bool material_paint = paint_mode_settings.canvas_source ==
                              PAINT_CANVAS_SOURCE_MATERIAL_PAINT;
  /* Poly Paint names its Base Color attribute in the mode settings; the Color Attribute canvas
   * follows the mesh's active-attribute pointer. */
  const StringRef name = material_paint ?
                             BKE_paint_material_channel_attribute_name(
                                 paint_mode_settings, PAINT_MATERIAL_CHANNEL_BASE_COLOR) :
                             StringRef(mesh.active_color_attribute);
  if (name.is_empty() && !material_paint) {
    /* The Color Attribute canvas has nothing else to fall back on. Poly Paint does: its scalar
     * channels are independent of whether Base Color resolves. */
    return nullptr;
  }
  const std::optional<bke::AttributeMetaData> meta =
      name.is_empty() ? std::nullopt : mesh.attributes().lookup_meta_data(name);
  /* For Poly Paint a missing attribute is not fatal on its own -- Base Color may simply be
   * disabled while scalar channels are on. The patch deliberately does NOT create anything:
   * attribute creation has to be part of an undo step, which is why the stroke engine does it
   * in `brush_stroke_init()`. Every interactive Curve Patch is spawned BY such a stroke, so by
   * the time a session starts the attributes its brush needs are already there. */
  const bool has_color = meta.has_value() &&
                         bke::mesh::is_color_attribute({meta->domain, meta->data_type});
  if (!material_paint && !has_color) {
    return nullptr;
  }

  /* The enabled scalar channels, resolved from the same brush the session is about to paint
   * with. `StrokeCache` is where that brush lives at this point: the interactive path hands the
   * session the spawning stroke's cache, and the operator path fills it in before publishing.
   * Frozen here rather than re-read per pass, so the set of attributes a patch owns cannot
   * change under its own snapshot. */
  Vector<ScalarTarget> scalar_targets;
  if (material_paint) {
    const SculptSession *ss = ob.runtime->sculpt_session;
    const StrokeCache *cache = ss != nullptr ? ss->cache : nullptr;
    const Brush *brush = cache != nullptr ? cache->brush : nullptr;
    const Paint *paint = cache != nullptr ? cache->paint : nullptr;
    if (brush != nullptr && brush->material_paint != nullptr && paint != nullptr) {
      for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
        /* Same predicate as #material::enabled_scalar_attribute_names, which is what the
         * stroke's undo push derives its list from -- the two must not disagree about which
         * attributes a Poly Paint write touches. */
        if (info.is_color || !info.supports_vertex_paint ||
            !BKE_paint_material_channel_writes_to_target(*brush->material_paint,
                                                         paint_mode_settings,
                                                         paint->visible_material_channels,
                                                         info.channel))
        {
          continue;
        }
        /* Normal is map-only (Image Texture -> Normal Map); it has no vertex float attribute.
         * Mirrors the same skip in `do_paint_material_brush()`. */
        if (info.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
          continue;
        }
        const StringRef attr_name = BKE_paint_material_channel_attribute_name(paint_mode_settings,
                                                                              info.channel);
        if (!mesh.attributes().lookup_meta_data(attr_name).has_value()) {
          continue;
        }
        /* A Custom channel pointed at one of the fixed attributes would otherwise be painted
         * (and snapshotted) twice -- the same duplicate `enabled_scalar_attribute_names()`
         * guards against for undo. */
        bool duplicate = false;
        for (const ScalarTarget &existing : scalar_targets) {
          duplicate |= existing.name == attr_name;
        }
        if (duplicate) {
          continue;
        }
        ScalarTarget target;
        target.name = attr_name;
        target.channel = info.channel;
        scalar_targets.append(std::move(target));
      }
    }
  }

  if (!has_color && scalar_targets.is_empty()) {
    /* Nothing enabled that this object can carry. */
    return nullptr;
  }
  return std::make_unique<ColorEffect>(
      has_color ? std::string(name) : std::string(),
      has_color ? meta->domain : bke::AttrDomain::Point,
      material_paint ? ColorTarget::MaterialChannel : ColorTarget::ActiveAttribute,
      material_paint ? &paint_mode_settings : nullptr,
      std::move(scalar_targets));
}

}  // namespace blender::ed::sculpt_paint
