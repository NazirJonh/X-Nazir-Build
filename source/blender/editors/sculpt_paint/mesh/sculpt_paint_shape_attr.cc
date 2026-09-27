/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Vertex-attribute write backends of the shape drawing tools: the Material Paint PBR channel
 * attributes and the Color Attribute canvas. Split out of `sculpt_paint_shape.cc`; see
 * `sculpt_paint_shape_intern.hh`.
 */

#include "sculpt_paint_shape_intern.hh"

#include "BLI_time.h"

namespace blender::ed::sculpt_paint::shape {

/** One vertex attribute the PBR bake writes: its channel, whether it is a color attribute and
 * the resolved attribute name. */
struct ShapeAttrTarget {
  eMaterialPaintChannel channel;
  bool is_color;
  std::string name;
};

/** Attributes the PBR bake will write, resolved from the brush/shape channel setup. Missing
 * attributes are created (and recorded in #r_created for undo) so a first draw on a fresh mesh
 * still paints. */
static Vector<ShapeAttrTarget> shape_attr_targets_resolve(
    ReportList *reports,
    Mesh &mesh,
    const PaintModeSettings &mode_settings,
    Paint &paint,
    const PaintShapeSettings &settings,
    Vector<std::string> &r_created,
    bool &r_any_created)
{
  Vector<ShapeAttrTarget> targets;
  /* Same resolver as the 2D compositor, restricted to the channels a vertex attribute can hold. */
  const uint32_t channels = BKE_paint_shape_target_channels(
      paint, mode_settings, settings, eShapeTargetKind::VertexAttributes);

  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    const eMaterialPaintChannel channel = info.channel;
    if ((channels & (1u << int(channel))) == 0) {
      continue;
    }
    const StringRef name = BKE_paint_material_channel_attribute_name(mode_settings, channel);
    if (name.is_empty()) {
      continue;
    }

    bool created = false;
    const MaterialPaintAttributeStatus status =
        info.is_color ?
            BKE_paint_mesh_material_color_attribute_ensure_named(mesh, name, &created) :
            BKE_paint_mesh_material_attribute_ensure(mesh, name, &created);
    if (status != MaterialPaintAttributeStatus::Ok) {
      if (reports != nullptr) {
        BKE_reportf(reports,
                    RPT_WARNING,
                    "%s channel: %s",
                    IFACE_(info.ui_name),
                    TIP_(BKE_paint_material_attribute_status_message(status)));
      }
      continue;
    }

    if (info.is_color && channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR &&
        mode_settings.channel_layer_bindings[PAINT_MATERIAL_CHANNEL_BASE_COLOR].attribute_name[0] ==
            '\0')
    {
      /* Workbench renders the mesh's active color attribute, so a redirected Base Color that
       * was not explicit is made visible, matching the material brush's stroke setup. */
      BKE_id_attributes_active_color_set(&mesh.id, name);
      if (created) {
        BKE_id_attributes_default_color_set(&mesh.id, name);
      }
    }

    if (created) {
      r_created.append(std::string(name));
    }
    r_any_created |= created;
    targets.append(ShapeAttrTarget{channel, info.is_color, std::string(name)});
  }
  return targets;
}

/** Alpha value the shape writes at \a sample: the Alpha channel's coverage of the strongest
 * part, or 1 when the channel is not active. */
static bool shape_alpha_coverage(const ShapeStyle &style,
                                 const ShapeSample &sample,
                                 const bool use_alpha_mask,
                                 float &r_alpha)
{
  if (!use_alpha_mask) {
    r_alpha = 1.0f;
    return true;
  }
  const ChannelWrite fill_alpha = shade_channel(
      style, sample, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_ALPHA);
  const ChannelWrite stroke_alpha = shade_channel(
      style, sample, ShapePart::Stroke, PAINT_MATERIAL_CHANNEL_ALPHA);
  r_alpha = std::max(fill_alpha.alpha, stroke_alpha.alpha);
  return r_alpha > 0.0f;
}

static void shape_write_color_attribute(const Array<ShapeVertexSample> &samples,
                                        const Vector<int> &candidates,
                                        const Array<int> &node_offset,
                                        MutableSpan<bke::pbvh::MeshNode> nodes,
                                        bke::GSpanAttributeWriter &writer,
                                        const OffsetIndices<int> faces,
                                        const Span<int> corner_verts,
                                        const GroupedSpan<int> vert_to_face_map,
                                        const FaceSelectionMask &face_selection_mask,
                                        const ShapeStyle &style,
                                        const eMaterialPaintChannel channel,
                                        const bool use_alpha_mask)
{
  threading::parallel_for(candidates.index_range(), 1, [&](const IndexRange node_range) {
    for (const int ci : node_range) {
      const Span<int> verts = nodes[candidates[ci]].verts();
      const Span<ShapeVertexSample> node_samples = samples.as_span().slice(node_offset[ci],
                                                                           verts.size());
      for (const int k : verts.index_range()) {
        const ShapeVertexSample &sample = node_samples[k];
        if (sample.factor <= 0.0f) {
          continue;
        }
        const int vert = verts[k];

        float alpha_cov;
        if (!shape_alpha_coverage(style, sample.sample, use_alpha_mask, alpha_cov)) {
          continue;
        }

        float4 result = color::color_vert_get(faces,
                                             corner_verts,
                                             vert_to_face_map,
                                             writer.span,
                                             writer.domain,
                                             vert,
                                             face_selection_mask.select_poly);
        for (const ShapePart part : {ShapePart::Fill, ShapePart::Stroke}) {
          const ChannelWrite write = shade_channel(style, sample.sample, part, channel);
          const float alpha = write.alpha * sample.factor * alpha_cov;
          if (alpha <= 0.0f) {
            continue;
          }
          const float4 mix(
              write.value.x * alpha, write.value.y * alpha, write.value.z * alpha, alpha);
          result = material::composite_coverage(result, mix, write.blend_mode);
        }
        color::color_vert_set(faces,
                              corner_verts,
                              vert_to_face_map,
                              writer.domain,
                              vert,
                              result,
                              writer.span,
                              face_selection_mask.select_poly);
      }
    }
  });
}

static void shape_write_scalar_attribute(const Array<ShapeVertexSample> &samples,
                                         const Vector<int> &candidates,
                                         const Array<int> &node_offset,
                                         MutableSpan<bke::pbvh::MeshNode> nodes,
                                         bke::SpanAttributeWriter<float> &writer,
                                         const ShapeStyle &style,
                                         const eMaterialPaintChannel channel,
                                         const float2 value_range,
                                         const bool use_alpha_mask)
{
  const MutableSpan<float> values = writer.span;
  threading::parallel_for(candidates.index_range(), 1, [&](const IndexRange node_range) {
    for (const int ci : node_range) {
      const Span<int> verts = nodes[candidates[ci]].verts();
      const Span<ShapeVertexSample> node_samples = samples.as_span().slice(node_offset[ci],
                                                                           verts.size());
      for (const int k : verts.index_range()) {
        const ShapeVertexSample &sample = node_samples[k];
        if (sample.factor <= 0.0f) {
          continue;
        }
        const int vert = verts[k];

        float alpha_cov;
        if (!shape_alpha_coverage(style, sample.sample, use_alpha_mask, alpha_cov)) {
          continue;
        }

        float result = values[vert];
        for (const ShapePart part : {ShapePart::Fill, ShapePart::Stroke}) {
          const ChannelWrite write = shade_channel(style, sample.sample, part, channel);
          const float alpha = write.alpha * sample.factor * alpha_cov;
          if (alpha <= 0.0f) {
            continue;
          }
          result = material::apply_scalar_blend(
              result, float2(write.value.x, alpha), write.blend_mode);
        }
        values[vert] = math::clamp(result, value_range.x, value_range.y);
      }
    }
  });
}

/* -------------------------------------------------------------------- */
/** \name Live-preview vertex snapshot
 * \{ */

/** Old values of the candidate vertices of one attribute, captured before the first preview write
 * and restored on #cancel / the next preview (no undo). The attribute-backend analogue of the
 * image backend's tile backups. */
struct ShapeAttrSnapshot {
  bool is_color = false;
  std::string name;
  Vector<int> verts;
  Vector<float4> colors;
  Vector<float> scalars;
};

using ShapeAttrSnapshots = Vector<ShapeAttrSnapshot>;

static void shape_attr_capture_color(const Vector<int> &candidates,
                                     MutableSpan<bke::pbvh::MeshNode> nodes,
                                     bke::GSpanAttributeWriter &writer,
                                     const OffsetIndices<int> faces,
                                     const Span<int> corner_verts,
                                     const GroupedSpan<int> vert_to_face_map,
                                     const FaceSelectionMask &face_selection_mask,
                                     const StringRef name,
                                     ShapeAttrSnapshots &r_snapshots)
{
  ShapeAttrSnapshot snapshot;
  snapshot.is_color = true;
  snapshot.name = name;
  for (const int ci : candidates.index_range()) {
    for (const int vert : nodes[candidates[ci]].verts()) {
      snapshot.verts.append(vert);
      snapshot.colors.append(color::color_vert_get(faces,
                                                   corner_verts,
                                                   vert_to_face_map,
                                                   writer.span,
                                                   writer.domain,
                                                   vert,
                                                   face_selection_mask.select_poly));
    }
  }
  r_snapshots.append(std::move(snapshot));
}

static void shape_attr_capture_scalar(const Vector<int> &candidates,
                                      MutableSpan<bke::pbvh::MeshNode> nodes,
                                      bke::SpanAttributeWriter<float> &writer,
                                      const StringRef name,
                                      ShapeAttrSnapshots &r_snapshots)
{
  ShapeAttrSnapshot snapshot;
  snapshot.is_color = false;
  snapshot.name = name;
  const Span<float> values = writer.span;
  for (const int ci : candidates.index_range()) {
    for (const int vert : nodes[candidates[ci]].verts()) {
      snapshot.verts.append(vert);
      snapshot.scalars.append(values[vert]);
    }
  }
  r_snapshots.append(std::move(snapshot));
}

/** Write the snapshot values back (no free) and tag the object geometry for redraw. */
static void shape_attr_restore(Object &ob, Mesh &mesh, ShapeAttrSnapshots &r_snapshots)
{
  if (r_snapshots.is_empty()) {
    return;
  }
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  FaceSelectionMask face_selection_mask;
  face_selection_mask_build(mesh, face_selection_mask);

  for (ShapeAttrSnapshot &snapshot : r_snapshots) {
    if (snapshot.is_color) {
      bke::GSpanAttributeWriter writer = attributes.lookup_for_write_span(snapshot.name);
      if (!writer) {
        continue;
      }
      for (const int i : snapshot.verts.index_range()) {
        color::color_vert_set(faces,
                              corner_verts,
                              vert_to_face_map,
                              writer.domain,
                              snapshot.verts[i],
                              snapshot.colors[i],
                              writer.span,
                              face_selection_mask.select_poly);
      }
      writer.finish();
    }
    else {
      bke::SpanAttributeWriter<float> writer = attributes.lookup_for_write_span<float>(snapshot.name);
      if (!writer) {
        continue;
      }
      for (const int i : snapshot.verts.index_range()) {
        writer.span[snapshot.verts[i]] = snapshot.scalars[i];
      }
      writer.finish();
    }
  }
  r_snapshots.clear();
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
}

/** The per-target write loop of the Material Paint bake, shared by the one-shot and the live
 * preview. \a r_snapshots (when non-null) captures the candidate vertices' old values first. */
static void shape_material_paint_write(Object &ob,
                                       Mesh &mesh,
                                       const PaintModeSettings &mode_settings,
                                       const Vector<ShapeAttrTarget> &targets,
                                       const ShapeBakeData &bake,
                                       const IndexMask &node_mask,
                                       const ShapeStyle &style,
                                       ShapeAttrSnapshots *r_snapshots)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  MutableSpan<bke::pbvh::MeshNode> nodes = bke::object::pbvh_get(ob)->nodes<bke::pbvh::MeshNode>();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  FaceSelectionMask face_selection_mask;
  face_selection_mask_build(mesh, face_selection_mask);

  for (const ShapeAttrTarget &target : targets) {
    const float2 range = BKE_paint_material_channel_range(mode_settings, target.channel);
    const bool alpha_active = (style.stroke_channels[PAINT_MATERIAL_CHANNEL_ALPHA].use != 0) ||
                              (style.fill_channels[PAINT_MATERIAL_CHANNEL_ALPHA].use != 0);
    const bool use_alpha_mask = material::channel_uses_alpha_mask(alpha_active, target.channel);

    if (target.is_color) {
      bke::GSpanAttributeWriter writer = attributes.lookup_for_write_span(target.name);
      if (!writer) {
        continue;
      }
      if (r_snapshots != nullptr) {
        shape_attr_capture_color(bake.candidates,
                                 nodes,
                                 writer,
                                 faces,
                                 corner_verts,
                                 vert_to_face_map,
                                 face_selection_mask,
                                 target.name,
                                 *r_snapshots);
      }
      shape_write_color_attribute(bake.samples,
                                  bake.candidates,
                                  bake.node_offset,
                                  nodes,
                                  writer,
                                  faces,
                                  corner_verts,
                                  vert_to_face_map,
                                  face_selection_mask,
                                  style,
                                  target.channel,
                                  use_alpha_mask);
      writer.finish();
      bke::object::pbvh_get(ob)->tag_attribute_changed(node_mask, target.name);
    }
    else {
      bke::SpanAttributeWriter<float> writer = attributes.lookup_for_write_span<float>(
          target.name);
      if (!writer) {
        continue;
      }
      if (r_snapshots != nullptr) {
        shape_attr_capture_scalar(bake.candidates, nodes, writer, target.name, *r_snapshots);
      }
      shape_write_scalar_attribute(bake.samples,
                                   bake.candidates,
                                   bake.node_offset,
                                   nodes,
                                   writer,
                                   style,
                                   target.channel,
                                   range,
                                   use_alpha_mask);
      writer.finish();
      bke::object::pbvh_get(ob)->tag_attribute_changed(node_mask, target.name);
    }
  }
}

/** The Color Attribute bake loop, shared by the one-shot and the live preview. */
static bool shape_color_attribute_write(Object &ob,
                                        Mesh &mesh,
                                        const ShapeBakeData &bake,
                                        const IndexMask &node_mask,
                                        const ShapeStyle &style,
                                        ShapeAttrSnapshots *r_snapshots)
{
  bke::GSpanAttributeWriter writer = color::active_color_attribute_for_write(mesh);
  if (!writer) {
    return false;
  }

  MutableSpan<bke::pbvh::MeshNode> nodes = bke::object::pbvh_get(ob)->nodes<bke::pbvh::MeshNode>();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  FaceSelectionMask face_selection_mask;
  face_selection_mask_build(mesh, face_selection_mask);

  if (r_snapshots != nullptr) {
    shape_attr_capture_color(bake.candidates,
                             nodes,
                             writer,
                             faces,
                             corner_verts,
                             vert_to_face_map,
                             face_selection_mask,
                             mesh.active_color_attribute,
                             *r_snapshots);
  }

  const bool fill_pass = style.use_fill();
  const bool stroke_pass = style.use_stroke() && style.stroke_width > 0.0f;
  threading::parallel_for(bake.candidates.index_range(), 1, [&](const IndexRange node_range) {
    for (const int ci : node_range) {
      const Span<int> verts = nodes[bake.candidates[ci]].verts();
      const Span<ShapeVertexSample> node_samples = bake.samples.as_span().slice(
          bake.node_offset[ci], verts.size());
      for (const int k : verts.index_range()) {
        const ShapeVertexSample &sample = node_samples[k];
        if (sample.factor <= 0.0f) {
          continue;
        }
        const int vert = verts[k];

        float4 result = color::color_vert_get(faces,
                                              corner_verts,
                                              vert_to_face_map,
                                              writer.span,
                                              writer.domain,
                                              vert,
                                              face_selection_mask.select_poly);
        for (const ShapePart part : {ShapePart::Fill, ShapePart::Stroke}) {
          if (part == ShapePart::Fill && !fill_pass) {
            continue;
          }
          if (part == ShapePart::Stroke && !stroke_pass) {
            continue;
          }
          const float coverage = shape_part_coverage(style, sample.sample, part);
          if (coverage <= 0.0f) {
            continue;
          }
          const float opacity = part == ShapePart::Stroke ? style.stroke_opacity :
                                                            style.fill_opacity;
          /* p = 0 for now: the gradient fill (fill_type) needs the shape's bounding box and its
           * own mapping, which land with D4. Until then a GRADIENT fill draws `fill_color`
           * because #shade_canvas ignores the position. */
          float4 color = shade_canvas(style, sample.sample, part, float2(0.0f));
          color.w *= opacity * coverage * sample.factor;
          if (color.w <= 0.0f) {
            continue;
          }
          const short blend = part == ShapePart::Stroke ? style.stroke_blend : style.fill_blend;
          const float4 mix(color.x * color.w, color.y * color.w, color.z * color.w, color.w);
          result = material::composite_coverage(result, mix, IMB_BlendMode(blend));
        }
        color::color_vert_set(faces,
                              corner_verts,
                              vert_to_face_map,
                              writer.domain,
                              vert,
                              result,
                              writer.span,
                              face_selection_mask.select_poly);
      }
    }
  });

  writer.finish();
  bke::object::pbvh_get(ob)->tag_attribute_changed(node_mask, mesh.active_color_attribute);
  return true;
}

/** \} */

static bool shape_apply_material_paint(bContext *C,
                                       ReportList *reports,
                                       const char *undo_name,
                                       Object &ob,
                                       Mesh &mesh,
                                       ToolSettings &toolsettings,
                                       Paint &paint,
                                       const PaintShapeSettings &settings,
                                       const ShapeBakeData &bake,
                                       const IndexMask &node_mask,
                                       const Depsgraph &depsgraph,
                                       ShapeStyle style)
{
  PaintModeSettings &mode_settings = toolsettings.paint_mode;
  style_channels_from_brush(paint, style);

  Vector<std::string> created_names_storage;
  bool any_created = false;
  const Vector<ShapeAttrTarget> targets = shape_attr_targets_resolve(
      reports, mesh, mode_settings, paint, settings, created_names_storage, any_created);
  if (targets.is_empty()) {
    if (reports != nullptr) {
      BKE_report(reports, RPT_INFO, "Paint Shape: no enabled PBR channel to paint");
    }
    return false;
  }

  /* Obtained after every attribute creation above, so the accessor's cached layers are current. */
  mesh.attributes_for_write();

  undo::push_begin_ex(*CTX_data_scene(C), ob, undo_name);
  {
    Vector<StringRef> scalar_names;
    Vector<StringRef> color_names;
    for (const ShapeAttrTarget &target : targets) {
      (target.is_color ? color_names : scalar_names).append(target.name);
    }
    Vector<StringRef> created_refs;
    for (const std::string &name : created_names_storage) {
      created_refs.append(name);
    }
    const undo::MaterialUndoAttributes material_attributes{
        scalar_names.as_span(), color_names.as_span(), created_refs.as_span()};
    undo::push_nodes(depsgraph, ob, node_mask, undo::NodeDataFlag::Material, material_attributes);
  }

  shape_material_paint_write(ob, mesh, mode_settings, targets, bake, node_mask, style, nullptr);

  undo::push_end_all_ex(false, true);

  if (any_created) {
    DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  }
  return true;
}

static bool shape_apply_color_attribute(bContext *C,
                                        ReportList *reports,
                                        const char *undo_name,
                                        Object &ob,
                                        Mesh &mesh,
                                        const ShapeBakeData &bake,
                                        const IndexMask &node_mask,
                                        const Depsgraph &depsgraph,
                                        const ShapeStyle &style)
{
  if (mesh.active_color_attribute == nullptr || mesh.active_color_attribute[0] == '\0') {
    if (reports != nullptr) {
      BKE_report(reports, RPT_WARNING, "Paint Shape: the mesh has no active color attribute");
    }
    return false;
  }

  undo::push_begin_ex(*CTX_data_scene(C), ob, undo_name);
  undo::push_nodes(depsgraph, ob, node_mask, undo::NodeDataFlag::Color);

  const bool ok = shape_color_attribute_write(ob, mesh, bake, node_mask, style, nullptr);

  undo::push_end_all_ex(false, true);
  return ok;
}

struct SculptMaterialPaintBackend::Data {
  ShapeAttrSnapshots snapshots;
  double last_tag = 0.0;
};

SculptMaterialPaintBackend::SculptMaterialPaintBackend(Sculpt3DTargetContext ctx)
    : ctx_(ctx), data_(std::make_unique<Data>())
{
}

SculptMaterialPaintBackend::~SculptMaterialPaintBackend()
{
  /* Reaching here with a live snapshot is a lifecycle error: every session end must cancel or
   * commit first. Restore best-effort so a forgotten path cannot leave the preview values. */
  BLI_assert(data_->snapshots.is_empty());
  if (!data_->snapshots.is_empty() && ctx_.ob != nullptr && ctx_.ob->data != nullptr) {
    shape_attr_restore(*ctx_.ob, *id_cast<Mesh *>(ctx_.ob->data), data_->snapshots);
  }
}

bool SculptMaterialPaintBackend::begin(bContext &C, ReportList * /*reports*/)
{
  composite_targets_ensure_writable(&C, ctx_.ob, *ctx_.paint, *ctx_.settings);
  return true;
}

bool SculptMaterialPaintBackend::preview(bContext *C,
                                         ReportList *reports,
                                         const Span<PaintShape> shapes,
                                         const ShapeStyle &style)
{
  const Depsgraph *depsgraph = ctx_.depsgraph_get(C);
  if (shapes.is_empty() || ctx_.ob == nullptr || ctx_.ob->data == nullptr || depsgraph == nullptr) {
    return false;
  }
  Mesh &mesh = *id_cast<Mesh *>(ctx_.ob->data);
  /* Restore the previous preview, then bake the new pose with a fresh snapshot. */
  shape_attr_restore(*ctx_.ob, mesh, data_->snapshots);

  ShapeBakeData bake;
  if (!shape_bake_prepare(*ctx_.ob, *ctx_.space, shapes, style, *depsgraph, bake, reports)) {
    return false;
  }
  IndexMaskMemory memory;
  const IndexMask node_mask = IndexMask::from_indices(bake.candidates.as_span(), memory);

  PaintModeSettings &mode_settings = ctx_.toolsettings->paint_mode;
  ShapeStyle bake_style = style;
  style_channels_from_brush(*ctx_.paint, bake_style);
  Vector<std::string> created_names_storage;
  bool any_created = false;
  const Vector<ShapeAttrTarget> targets = shape_attr_targets_resolve(reports,
                                                                     mesh,
                                                                     mode_settings,
                                                                     *ctx_.paint,
                                                                     *ctx_.settings,
                                                                     created_names_storage,
                                                                     any_created);
  if (targets.is_empty()) {
    return false;
  }
  shape_material_paint_write(
      *ctx_.ob, mesh, mode_settings, targets, bake, node_mask, bake_style, &data_->snapshots);
  this->tag_preview(C, false);
  return true;
}

bool SculptMaterialPaintBackend::commit(bContext *C,
                                        ReportList *reports,
                                        const Span<PaintShape> shapes,
                                        const ShapeStyle &style,
                                        const char *undo_name)
{
  if (C == nullptr) {
    return false;
  }
  /* Drop the live preview (restore + free) before the undo-captured bake. */
  this->cancel();

  const Depsgraph *depsgraph = ctx_.depsgraph_get(C);
  ShapeBakeData bake;
  if (depsgraph == nullptr ||
      !shape_bake_prepare(*ctx_.ob, *ctx_.space, shapes, style, *depsgraph, bake, reports))
  {
    return false;
  }
  IndexMaskMemory memory;
  const IndexMask node_mask = IndexMask::from_indices(bake.candidates.as_span(), memory);
  Mesh &mesh = *id_cast<Mesh *>(ctx_.ob->data);
  if (!shape_apply_material_paint(C,
                                  reports,
                                  undo_name,
                                  *ctx_.ob,
                                  mesh,
                                  *ctx_.toolsettings,
                                  *ctx_.paint,
                                  *ctx_.settings,
                                  bake,
                                  node_mask,
                                  *depsgraph,
                                  style))
  {
    return false;
  }
  flush_update_step(C, UpdateType::Color);
  flush_update_done(C, *ctx_.ob, UpdateType::Color);
  return true;
}

void SculptMaterialPaintBackend::cancel()
{
  if (!data_->snapshots.is_empty() && ctx_.ob != nullptr && ctx_.ob->data != nullptr) {
    shape_attr_restore(*ctx_.ob, *id_cast<Mesh *>(ctx_.ob->data), data_->snapshots);
  }
  data_->snapshots.clear();
}

void SculptMaterialPaintBackend::preview_force_update(bContext *C)
{
  this->tag_preview(C, true);
}

void SculptMaterialPaintBackend::tag_preview(bContext *C, const bool force)
{
  const double now = BLI_time_now_seconds();
  if (!force && now - data_->last_tag < SHAPE_PREVIEW_SHADING_TAG_INTERVAL) {
    return;
  }
  data_->last_tag = now;
  if (ctx_.ob != nullptr) {
    DEG_id_tag_update(&ctx_.ob->id, ID_RECALC_GEOMETRY);
    if (C != nullptr) {
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ctx_.ob);
    }
  }
}

struct SculptColorAttributeBackend::Data {
  ShapeAttrSnapshots snapshots;
  double last_tag = 0.0;
};

SculptColorAttributeBackend::SculptColorAttributeBackend(Sculpt3DTargetContext ctx)
    : ctx_(ctx), data_(std::make_unique<Data>())
{
}

SculptColorAttributeBackend::~SculptColorAttributeBackend()
{
  BLI_assert(data_->snapshots.is_empty());
  if (!data_->snapshots.is_empty() && ctx_.ob != nullptr && ctx_.ob->data != nullptr) {
    shape_attr_restore(*ctx_.ob, *id_cast<Mesh *>(ctx_.ob->data), data_->snapshots);
  }
}

bool SculptColorAttributeBackend::begin(bContext & /*C*/, ReportList * /*reports*/)
{
  return true;
}

bool SculptColorAttributeBackend::preview(bContext *C,
                                          ReportList *reports,
                                          const Span<PaintShape> shapes,
                                          const ShapeStyle &style)
{
  const Depsgraph *depsgraph = ctx_.depsgraph_get(C);
  if (shapes.is_empty() || ctx_.ob == nullptr || ctx_.ob->data == nullptr || depsgraph == nullptr) {
    return false;
  }
  Mesh &mesh = *id_cast<Mesh *>(ctx_.ob->data);
  shape_attr_restore(*ctx_.ob, mesh, data_->snapshots);

  ShapeBakeData bake;
  if (!shape_bake_prepare(*ctx_.ob, *ctx_.space, shapes, style, *depsgraph, bake, reports)) {
    return false;
  }
  IndexMaskMemory memory;
  const IndexMask node_mask = IndexMask::from_indices(bake.candidates.as_span(), memory);
  if (!shape_color_attribute_write(*ctx_.ob, mesh, bake, node_mask, style, &data_->snapshots)) {
    return false;
  }
  this->tag_preview(C, false);
  return true;
}

bool SculptColorAttributeBackend::commit(bContext *C,
                                         ReportList *reports,
                                         const Span<PaintShape> shapes,
                                         const ShapeStyle &style,
                                         const char *undo_name)
{
  if (C == nullptr) {
    return false;
  }
  this->cancel();

  const Depsgraph *depsgraph = ctx_.depsgraph_get(C);
  ShapeBakeData bake;
  if (depsgraph == nullptr ||
      !shape_bake_prepare(*ctx_.ob, *ctx_.space, shapes, style, *depsgraph, bake, reports))
  {
    return false;
  }
  IndexMaskMemory memory;
  const IndexMask node_mask = IndexMask::from_indices(bake.candidates.as_span(), memory);
  Mesh &mesh = *id_cast<Mesh *>(ctx_.ob->data);
  if (!shape_apply_color_attribute(
          C, reports, undo_name, *ctx_.ob, mesh, bake, node_mask, *depsgraph, style))
  {
    return false;
  }
  flush_update_step(C, UpdateType::Color);
  flush_update_done(C, *ctx_.ob, UpdateType::Color);
  return true;
}

void SculptColorAttributeBackend::cancel()
{
  if (!data_->snapshots.is_empty() && ctx_.ob != nullptr && ctx_.ob->data != nullptr) {
    shape_attr_restore(*ctx_.ob, *id_cast<Mesh *>(ctx_.ob->data), data_->snapshots);
  }
  data_->snapshots.clear();
}

void SculptColorAttributeBackend::preview_force_update(bContext *C)
{
  this->tag_preview(C, true);
}

void SculptColorAttributeBackend::tag_preview(bContext *C, const bool force)
{
  const double now = BLI_time_now_seconds();
  if (!force && now - data_->last_tag < SHAPE_PREVIEW_SHADING_TAG_INTERVAL) {
    return;
  }
  data_->last_tag = now;
  if (ctx_.ob != nullptr) {
    DEG_id_tag_update(&ctx_.ob->id, ID_RECALC_GEOMETRY);
    if (C != nullptr) {
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ctx_.ob);
    }
  }
}

}  // namespace blender::ed::sculpt_paint::shape
