/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * PBVH-pixels write backend of the shape drawing tools: the Material Paint channel maps and the
 * Image canvas. Split out of `sculpt_paint_shape.cc`; see `sculpt_paint_shape_intern.hh`.
 */

#include "sculpt_paint_shape_intern.hh"

#include "BKE_lib_id.hh"

#include "BLT_translation.hh"

#include "BLI_time.h"

namespace blender::ed::sculpt_paint::shape {

/** One image the bake writes: its pixels, plus the channel the target map holds (-1 for the
 * plain Image canvas). The list comes from #composite_targets_get so it matches the 2D bake. */
struct ShapeImageTarget {
  std::unique_ptr<paint::image::ImageData> data;
  int channel = -1;
  bool is_normal = false;
  /** Channel data or layer mask; only #Channel exists here today. */
  ShapeTargetKind kind = ShapeTargetKind::Channel;
  /** UV map to project through; empty means the canvas UV (see the TODO below). */
  StringRef uv_map_name;
};

static Vector<ShapeImageTarget> shape_image_targets_get(bContext *C,
                                                        Object &ob,
                                                        Paint &paint,
                                                        const PaintShapeSettings &settings)
{
  Vector<ShapeImageTarget> targets;
  for (const ShapeTarget &shape_target : composite_targets_get(C, &ob, paint, settings)) {
    std::unique_ptr<paint::image::ImageData> data = paint::image::ImageData::from_image(
        shape_target.image, shape_target.iuser);
    if (!data) {
      continue;
    }
    ShapeImageTarget target;
    target.data = std::move(data);
    target.channel = shape_target.channel;
    target.kind = shape_target.kind;
    target.uv_map_name = shape_target.uv_map_name;
    if (shape_target.channel >= 0) {
      target.is_normal = shape_target.channel == PAINT_MATERIAL_CHANNEL_NORMAL;
    }
    targets.append(std::move(target));
  }
  return targets;
}

/** One tile the live preview overwrote: its full pixels before the first preview write. The
 * live #SculptImageBackend owns a list of these and restores them on #cancel / before the next
 * preview; the one-shot bake never creates any. */
struct PreviewTileBackup {
  int target_index = 0;
  int tile_number = 0;
  ImBuf *orig = nullptr;
  /** Union of every region the preview wrote on this tile (image pixels, half-open): the only part
   * a refresh has to restore, instead of the whole tile. Empty before the first write. */
  rcti dirty = {0, 0, 0, 0};
  /** The dirty region was restored but the seam-bleed pixels the previous write derived outside it
   * were not re-derived yet; the write pass clears it once #copy_pixels ran for the tile. */
  bool bleed_pending = false;
};

static PreviewTileBackup *preview_backup_find(Vector<PreviewTileBackup> &backups,
                                              const int target_index,
                                              const int tile_number)
{
  for (PreviewTileBackup &backup : backups) {
    if (backup.target_index == target_index && backup.tile_number == tile_number) {
      return &backup;
    }
  }
  return nullptr;
}

/** Back up \a tile_number of \a target once per (target, tile); a later preview reuses it. */
static void preview_tile_backup(Vector<PreviewTileBackup> &backups,
                                const int target_index,
                                const int tile_number,
                                const ShapeImageTarget &target)
{
  if (preview_backup_find(backups, target_index, tile_number) != nullptr) {
    return;
  }
  paint::image::ImageData &image_data = *target.data;
  if (ImBuf *orig = shape_tile_backup_init(
          image_data.image, image_data.image_user, tile_number))
  {
    backups.append(PreviewTileBackup{target_index, tile_number, orig});
  }
}

/**
 * MATERIAL / Image canvas backend. Every texel whose UV primitive is covered by the shape is
 * projected into the region, sampled through the shared #ShapeEvaluator, shaded by
 * `shape_blend_pixel` and written back through the PBVH-pixels tile plumbing.
 *
 * The whole bake is one image undo step (all maps together), matching the brush's texture-paint
 * canvas: `ED_image_undo_push_begin` / `do_push_undo_tile` / `ED_image_undo_push_end`.
 */
static bool shape_write_image(bContext *C,
                              ReportList *reports,
                              const char *undo_name,
                              Object &ob,
                              const Depsgraph &depsgraph,
                              ToolSettings &toolsettings,
                              const ShapeSpace &space,
                              Paint &paint,
                              const Span<ShapeImageTarget> targets,
                              const Span<PaintShape> shapes,
                              ShapeStyle style,
                              const bool preview,
                              Vector<PreviewTileBackup> *r_backups)
{
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    if (!preview && reports != nullptr) {
      BKE_report(reports, RPT_WARNING, "Paint Shape: the active object has no sculpt mesh");
    }
    return false;
  }
  ViewProjectorCamera camera;
  /* A SurfaceAnchored space has no view camera (occlusion is its #SurfaceAnchor::max_depth
   * instead); the projection matrices are still needed for the normal basis. */
  const bool has_camera = space.view_camera(ob, camera);
  float4x4 persmat;
  int2 win_size;
  if (!space.view_projection(persmat, win_size)) {
    if (!preview && reports != nullptr) {
      BKE_report(reports, RPT_WARNING, "Paint Shape: the shape space has no view");
    }
    return false;
  }

  style_channels_from_brush(paint, style);

  rctf domain;
  BLI_rctf_init(&domain, FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX);
  for (const PaintShape &shape : shapes) {
    const rctf bounds = shape_bounds_calc(shape, style);
    if (bounds.xmin > bounds.xmax) {
      continue;
    }
    domain.xmin = std::min(domain.xmin, bounds.xmin);
    domain.xmax = std::max(domain.xmax, bounds.xmax);
    domain.ymin = std::min(domain.ymin, bounds.ymin);
    domain.ymax = std::max(domain.ymax, bounds.ymax);
  }
  if (domain.xmin > domain.xmax) {
    if (!preview && reports != nullptr) {
      BKE_report(reports, RPT_WARNING, "Paint Shape: the shape has no area");
    }
    return false;
  }

  const bool alpha_active = (style.stroke_channels[PAINT_MATERIAL_CHANNEL_ALPHA].use != 0) ||
                            (style.fill_channels[PAINT_MATERIAL_CHANNEL_ALPHA].use != 0);

  /* Gradient fill: the union bounds of the shapes are the domain. One shape plus mesh symmetry is
   * the current case; several shapes would need a per-shape box like the 2D compositor (R6). */
  ShapeFillGradient fill_gradient;
  const ShapeFillGradient *gradient = nullptr;
  if (style.use_fill() && style.fill_type == PAINT_SHAPE_FILL_GRADIENT && style.use_fill_gradient) {
    fill_gradient = ShapeFillGradient{float2(domain.xmin, domain.ymin),
                                      float2(domain.xmax, domain.ymax)};
    gradient = &fill_gradient;
  }

  /* Normal and Height need the distance/direction outputs, so everything is requested here. */
  const ShapeEvaluator evaluator(shapes, style, domain, SHAPE_RASTER_OUTPUTS_ALL);

  Vector<ePaintSymmetryFlags> passes;
  const int symmetry_flags = int(mesh_symmetry_xyz_get(ob));
  for (int i = 0; i <= symmetry_flags; i++) {
    if (is_symmetry_iteration_valid(i, symmetry_flags)) {
      passes.append(ePaintSymmetryFlags(i));
    }
  }

  const ShapeViewBasis view_basis = space.view_basis(ob);
  /* Built once here: #vertex_is_occluded lazily creates it, and the per-texel parallel loop
   * below must not race on that. */
  vert_random_access_ensure(ob);

  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  MeshAttributeData attribute_data(mesh);
  FaceSelectionMask face_selection_mask;
  face_selection_mask_build(mesh, face_selection_mask);
  const bool use_face_selection = face_selection_mask.state == FaceSelectionState::Active;
  BLI_assert(!use_face_selection || face_selection_mask.vert_paintable.size() == mesh.verts_num);
  BLI_assert(!use_face_selection || face_selection_mask.select_poly.size() == mesh.faces_num);
  const Span<int> corner_tri_faces = mesh.corner_tri_faces();
  const Span<bool> hide_poly = attribute_data.hide_poly;
  const Span<float> mask = attribute_data.mask;

  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
  Vector<int> candidates;
  for (const int i : nodes.index_range()) {
    const Bounds<float3> &bounds = nodes[i].bounds();
    rctf node_rct;
    BLI_rctf_init(&node_rct, FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX);
    bool any_corner = false;
    for (int corner = 0; corner < 8; corner++) {
      const float3 p = bounds_corner(bounds, corner);
      for (const ePaintSymmetryFlags pass : passes) {
        const float3 flipped = symmetry_flip(p, pass);
        float2 co;
        if (!space.project_point(flipped, float3(0.0f), co)) {
          continue;
        }
        node_rct.xmin = std::min(node_rct.xmin, co.x);
        node_rct.xmax = std::max(node_rct.xmax, co.x);
        node_rct.ymin = std::min(node_rct.ymin, co.y);
        node_rct.ymax = std::max(node_rct.ymax, co.y);
        any_corner = true;
      }
    }
    if (!any_corner) {
      continue;
    }
    rctf isect;
    if (BLI_rctf_isect(&node_rct, &domain, &isect)) {
      candidates.append(i);
    }
  }
  if (candidates.is_empty()) {
    if (!preview && reports != nullptr) {
      BKE_report(reports, RPT_INFO, "Paint Shape: the shape does not reach the mesh");
    }
    return false;
  }

  if (targets.is_empty()) {
    /* Material mode refuses visibly instead of writing elsewhere; the Canvas / Image source
     * keeps its existing "no channel" info text. Preview failures stay silent. */
    if (!preview && reports != nullptr) {
      const char *refusal = shape_targets_refusal_message(C, &ob);
      BKE_report(reports,
                 refusal != nullptr ? RPT_WARNING : RPT_INFO,
                 refusal != nullptr ? refusal :
                                      RPT_("Paint Shape: no enabled image channel to paint"));
    }
    return false;
  }

  IndexMaskMemory memory;
  const IndexMask node_mask = IndexMask::from_indices(candidates.as_span(), memory);
  /* TODO: project through `target.uv_map_name` once the resolver fills it; empty means the
   * canvas UV, which is what this call returns today. */
  const StringRef uv_map_name = BKE_paint_canvas_uvmap_name_get(&toolsettings.paint_mode, &ob)
                                    .value_or("");

  std::atomic<bool> any_painted{false};
  Array<bool> target_painted(targets.size(), false);
  /* Per-bake context: the style and the alpha mask are constant, only the channel and the
   * per-primitive normal basis change. */
  ShapeBlendContext base_ctx;
  base_ctx.style = &style;
  base_ctx.alpha_active = alpha_active;
  base_ctx.fill_gradient = gradient;
  if (!preview) {
    ED_image_undo_push_begin(undo_name, PaintMode::Sculpt);
  }

  for (const int target_i : targets.index_range()) {
    const ShapeImageTarget &target = targets[target_i];
    paint::image::ImageData &image_data = *target.data;
    if (!bke::pbvh::build_pixels(
            depsgraph, ob, *image_data.image, *image_data.image_user, uv_map_name))
    {
      continue;
    }
    bke::pbvh::pixels::PixelData &pixel_data = bke::pbvh::pixels::data_get(*pbvh);
    MutableSpan<bke::pbvh::pixels::PixelNode> pixel_nodes = pixel_data.nodes;

    /* Sequential: #fetch_image_buffers inserts into non-thread-safe maps, and the tile backups
     * append to a shared vector and duplicate whole image buffers. */
    node_mask.foreach_index([&](const int i) {
      bke::pbvh::pixels::PixelNode &pixel_node = pixel_nodes[i];
      if (pixel_node.tiles.is_empty()) {
        return;
      }
      paint::image::fetch_image_buffers(image_data, nodes[i], pixel_node);
      if (preview) {
        /* Back up every tile of this node before it can be written; #cancel restores them and
         * the next preview restores + re-backs them first. */
        for (const bke::pbvh::pixels::UDIMTilePixels &tile : pixel_node.tiles) {
          preview_tile_backup(*r_backups, target_i, tile.tile_number, target);
        }
      }
    });

    node_mask.foreach_index(
        [&](const int i) {
          bke::pbvh::pixels::PixelNode &pixel_node = pixel_nodes[i];
          if (pixel_node.tiles.is_empty()) {
            return;
          }
          if (!preview) {
            paint::image::do_push_undo_tile(image_data, nodes[i], pixel_node);
          }

          for (bke::pbvh::pixels::UDIMTilePixels &tile : pixel_node.tiles) {
            ImBuf *image_buffer = image_data.buffers.lookup_default(tile.tile_number, nullptr);
            const paint::image::TileColorspaceProcessor *processors =
                image_data.processors.lookup_ptr(tile.tile_number);
            if (image_buffer == nullptr || processors == nullptr) {
              continue;
            }

            MutableSpan<float4> float_buffer;
            MutableSpan<uchar4> byte_buffer;
            if (image_buffer->float_data()) {
              float_buffer = MutableSpan(
                  reinterpret_cast<float4 *>(image_buffer->float_data_for_write()),
                  image_buffer->x * image_buffer->y);
            }
            else {
              byte_buffer = MutableSpan(
                  reinterpret_cast<uchar4 *>(image_buffer->byte_data_for_write()),
                  image_buffer->x * image_buffer->y);
            }

            Array<bool> rows_changed(tile.pixel_rows.size(), false);
            threading::parallel_for(
                tile.pixel_rows.index_range(), 64, [&](const IndexRange rows) {
                  Vector<float3> positions;
                  Vector<float4> byte_storage;
                  for (const int r : rows) {
                    const bke::pbvh::pixels::PackedPixelRow &pixel_row = tile.pixel_rows[r];
                    const int tri_local = pixel_row.uv_primitive_index;
                    BLI_assert(tri_local >= 0 &&
                               tri_local < pixel_node.uv_primitives.tri_indices.size());
                    BLI_assert(tri_local <
                               pixel_node.uv_primitives.delta_barycentric_coords.size());
                    const int tri = pixel_node.uv_primitives.tri_indices[tri_local];
                    BLI_assert(tri >= 0 && tri < pixel_data.vert_tris.size());
                    BLI_assert(tri < corner_tri_faces.size());
                    const int face = corner_tri_faces[tri];
                    if (!hide_poly.is_empty() && hide_poly[face]) {
                      continue;
                    }
                    if (use_face_selection && !face_selection_mask.select_poly[face]) {
                      continue;
                    }

                    const int3 tri_verts = pixel_data.vert_tris[tri];
                    const float3 v0 = vert_positions[tri_verts[0]];
                    const float3 v1 = vert_positions[tri_verts[1]];
                    const float3 v2 = vert_positions[tri_verts[2]];
                    const float3 tri_normal = math::normalize(
                        math::cross(v1 - v0, v2 - v0));
                    const float2 delta_bary = pixel_node.uv_primitives.delta_barycentric_coords
                                                  [tri_local];

                    NormalWriteBasis basis;
                    if (target.is_normal) {
                      BLI_assert(tri_local < pixel_node.uv_primitives.tangents.size());
                      BLI_assert(tri_local < pixel_node.uv_primitives.bitangent_signs.size());
                      BLI_assert(tri_local * 3 + 3 <=
                                 pixel_node.uv_primitives.triangle_positions.size());
                      const float3 tri_tangent = pixel_node.uv_primitives.tangents[tri_local];
                      const float tri_bitangent_sign =
                          pixel_node.uv_primitives.bitangent_signs[tri_local];
                      const Span<float3> tri_positions =
                          pixel_node.uv_primitives.triangle_positions.as_span().slice(
                              tri_local * 3, 3);
                      material::build_normal_write_basis(tri_tangent,
                                                         tri_bitangent_sign,
                                                         tri_positions,
                                                         view_basis.view_right,
                                                         win_size,
                                                         persmat,
                                                         basis.t_screen,
                                                         basis.b_screen,
                                                         basis.n_m,
                                                         basis.t_m,
                                                         basis.b_m);
                    }
                    ShapeBlendContext row_ctx = base_ctx;
                    row_ctx.channel = target.channel;
                    row_ctx.normal_basis = target.is_normal ? &basis : nullptr;

                    const IndexRange range(0, pixel_row.num_pixels);
                    positions.resize(range.size());
                    paint::image::calc_pixel_row_positions(
                        vert_positions,
                        pixel_data.vert_tris,
                        pixel_node.uv_primitives.tri_indices,
                        pixel_node.uv_primitives.delta_barycentric_coords,
                        pixel_row,
                        range,
                        positions);

                    const int64_t buffer_row_offset =
                        int64_t(pixel_row.start_image_coordinate.y) * image_buffer->x +
                        pixel_row.start_image_coordinate.x;
                    BLI_assert(buffer_row_offset >= 0 &&
                               buffer_row_offset + pixel_row.num_pixels <=
                                   int64_t(image_buffer->x) * image_buffer->y);

                    const MutableSpan<float4> dst =
                        !float_buffer.is_empty() ?
                            paint::image::read_image_pixels(
                                float_buffer, *processors, pixel_row, range, image_buffer->x) :
                            paint::image::read_image_pixels(byte_buffer,
                                                            *processors,
                                                            pixel_row,
                                                            range,
                                                            image_buffer->x,
                                                            byte_storage);

                    bool changed = false;
                    for (const int px : range.index_range()) {
                      const float2 bary = pixel_row.start_barycentric_coord +
                                          delta_bary * float(px);
                      const float w0 = bary.x;
                      const float w1 = bary.y;
                      const float w2 = 1.0f - w0 - w1;
                      float vertex_mask = 0.0f;
                      if (!mask.is_empty()) {
                        vertex_mask = w0 * mask[tri_verts[0]] + w1 * mask[tri_verts[1]] +
                                      w2 * mask[tri_verts[2]];
                      }
                      const float factor = 1.0f - math::clamp(vertex_mask, 0.0f, 1.0f);
                      if (factor <= 0.0f) {
                        continue;
                      }

                      const float3 position = positions[px];
                      float best_score = 0.0f;
                      float3 best_position = float3(0.0f);
                      float2 best_co = float2(0.0f);
                      ShapeSample best;
                      for (const ePaintSymmetryFlags pass : passes) {
                        const float3 mirrored = symmetry_flip(position, pass);
                        const float3 mirrored_normal = symmetry_flip(tri_normal, pass);
                        if (!view_basis.front_facing(mirrored, mirrored_normal)) {
                          continue;
                        }
                        float2 co;
                        if (!space.project_point(mirrored, mirrored_normal, co)) {
                          continue;
                        }
                        const ShapeSample candidate = evaluator.sample(co);
                        const float score = std::max(candidate.fill, candidate.stroke);
                        if (score > best_score) {
                          best_score = score;
                          best = candidate;
                          best_position = mirrored;
                          best_co = co;
                        }
                      }
                      if (best_score <= 0.0f) {
                        continue;
                      }
                      if (has_camera && vertex_is_occluded(depsgraph, ob, camera, best_position, false)) {
                        continue;
                      }
                      shape_blend_pixel(dst[px], row_ctx, best, factor, best_co);
                      changed = true;
                    }

                    if (!changed) {
                      continue;
                    }
                    if (!float_buffer.is_empty()) {
                      paint::image::write_image_pixels(
                          dst, float_buffer, *processors, pixel_row, range, image_buffer->x);
                    }
                    else {
                      paint::image::write_image_pixels(
                          dst, byte_buffer, *processors, pixel_row, range, image_buffer->x);
                    }
                    rows_changed[r] = true;
                  }
                });

            bool tile_touched = false;
            for (const int r : tile.pixel_rows.index_range()) {
              if (!rows_changed[r]) {
                continue;
              }
              const bke::pbvh::pixels::PackedPixelRow &pixel_row = tile.pixel_rows[r];
              const int2 start(pixel_row.start_image_coordinate.x,
                               pixel_row.start_image_coordinate.y);
              const int2 end = start + int2(pixel_row.num_pixels + 1, 1);
              tile.mark_dirty(Bounds<int2>(start, end));
              tile_touched = true;
              any_painted.store(true, std::memory_order_relaxed);
            }
            if (tile_touched) {
              pixel_node.flags.dirty = true;
              target_painted[target_i] = true;
            }
            if (tile.flags.dirty) {
              /* Mirror `mark_gpu_texture_regions_dirty` + the per-dab `BKE_image_mark_dirty`
               * from `sculpt_paint_image.cc`: the delayed GPU upload records the rectangle in
               * the image's partial-update log that the material/GPU caches subscribe to, and
               * the dirty mark flags the buffer as modified. */
              ImageTile *image_tile = BKE_image_get_tile(image_data.image, tile.tile_number);
              if (image_tile != nullptr && image_buffer->color_mode != ImColorMode::BW) {
                BKE_image_update_gputexture_delayed(image_data.image,
                                                    image_tile,
                                                    image_buffer,
                                                    tile.dirty_region.xmin,
                                                    tile.dirty_region.ymin,
                                                    BLI_rcti_size_x(&tile.dirty_region),
                                                    BLI_rcti_size_y(&tile.dirty_region));
              }
              /* The live preview must not flag the image as modified; the delayed GPU upload
               * above is what makes it visible. The commit marks it dirty. */
              if (!preview) {
                BKE_image_mark_dirty(image_data.image, image_buffer);
              }
            }
          }
        },
        exec_mode::grain_size(1));

    /* The dirty-tile set must be read before #mark_image_dirty clears each node's dirty flag. */
    Vector<bke::image::TileNumber> dirty_tiles = paint::image::collect_dirty_tiles(pixel_nodes,
                                                                                     node_mask);
    if (preview && r_backups != nullptr) {
      /* Sequential: grow each backup's dirty region by what this write touched, so the next
       * refresh restores exactly that instead of the whole tile. */
      node_mask.foreach_index([&](const int i) {
        for (const bke::pbvh::pixels::UDIMTilePixels &tile : pixel_nodes[i].tiles) {
          if (BLI_rcti_is_empty(&tile.dirty_region)) {
            continue;
          }
          if (PreviewTileBackup *backup = preview_backup_find(
                  *r_backups, target_i, tile.tile_number))
          {
            shape_region_union(backup->dirty, tile.dirty_region);
          }
        }
      });
      /* A region restore leaves the seam pixels the previous write derived outside it: re-derive
       * them for every such tile that has a buffer, from the now-current source pixels. */
      for (PreviewTileBackup &backup : *r_backups) {
        if (backup.target_index == target_i && backup.bleed_pending &&
            image_data.buffers.lookup_default(backup.tile_number, nullptr) != nullptr)
        {
          dirty_tiles.append(backup.tile_number);
          backup.bleed_pending = false;
        }
      }
    }
    if (!dirty_tiles.is_empty()) {
      paint::image::fix_non_manifold_seam_bleeding(*pbvh, image_data.buffers, dirty_tiles);
    }
    if (!preview) {
      node_mask.foreach_index([&](const int i) {
        bke::pbvh::pixels::mark_image_dirty(
            nodes[i], pixel_nodes[i], *image_data.image, image_data.buffers);
      });
    }
  }

  if (!preview && ED_image_undo_is_step_active()) {
    ED_image_undo_push_end();
  }

  if (!any_painted.load()) {
    if (!preview && reports != nullptr) {
      BKE_report(reports, RPT_INFO, "Paint Shape: no visible, unmasked texel in the shape");
    }
    return false;
  }

  /* Tell every editor showing the maps and the material's shading that the pixels changed.
   * Copied from the 2D paint finish (`paint_2d_redraw`, paint_image_2d.cc:5163-5190): the
   * per-image edited notifier + shading tags, plus the object shading tag (the image is wired
   * into the Principled BSDF, but the 3D Viewport's GPU material is not refreshed by an image
   * tag alone). The live preview emits the same tags through #SculptImageBackend::tag_preview,
   * throttled, so the shared write does it only for a bake. */
  if (!preview) {
    for (const int target_i : targets.index_range()) {
      if (!target_painted[target_i]) {
        continue;
      }
      Image *image = targets[target_i].data->image;
      WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, image);
      DEG_id_tag_update(&image->id, ID_RECALC_SHADING | ID_RECALC_PARAMETERS);
    }
    DEG_id_tag_update(&ob.id, ID_RECALC_SHADING);

    flush_update_step(C, UpdateType::Image);
    flush_update_done(C, ob, UpdateType::Image);
  }
  /* The owner region redraw + notifier are the dispatcher's job (it owns the ARegion). */
  return true;
}

/** Live preview of the Image canvas: resolved targets plus the full-tile backups the session
 * restores. Mirrors #ImageTilesBackend: the write happens every call, only the shading tag is
 * throttled, and the backups live until #commit / #cancel / the destructor. */
struct SculptImageBackend::Data {
  Vector<ShapeImageTarget> targets;
  /** `session_uid` of every target Image at #begin, parallel to #targets: the liveness lookup
   * goes through it, so a stored (possibly dangling) Image pointer is never dereferenced. */
  Vector<uint32_t> target_uids;
  Main *bmain = nullptr;
  Vector<PreviewTileBackup> backups;
  double last_tag = 0.0;
};

SculptImageBackend::SculptImageBackend(Sculpt3DTargetContext ctx)
    : ctx_(ctx), data_(std::make_unique<Data>())
{
}

SculptImageBackend::~SculptImageBackend()
{
  /* Reaching here with live backups is a lifecycle error: every session end must call #cancel or
   * #commit first. Restore best-effort so a forgotten path cannot leave the preview pixels in the
   * image, then free. */
  BLI_assert(data_->backups.is_empty());
  this->restore_backups(true);
}

void SculptImageBackend::target_images(Vector<const Image *> &r_images) const
{
  if (data_ == nullptr) {
    return;
  }
  for (const ShapeImageTarget &target : data_->targets) {
    if (target.data != nullptr && target.data->image != nullptr &&
        !r_images.contains(target.data->image))
    {
      r_images.append(target.data->image);
    }
  }
}

bool SculptImageBackend::begin(bContext &C, ReportList *reports)
{
  composite_targets_ensure_writable(&C, ctx_.ob, *ctx_.paint, *ctx_.settings);
  data_->targets = shape_image_targets_get(&C, *ctx_.ob, *ctx_.paint, *ctx_.settings);
  if (data_->targets.is_empty()) {
    if (const char *reason = shape_targets_refusal_message(&C, ctx_.ob)) {
      if (reports != nullptr) {
        BKE_report(reports, RPT_WARNING, reason);
      }
    }
    return false;
  }
  data_->bmain = CTX_data_main(&C);
  for (const ShapeImageTarget &target : data_->targets) {
    data_->target_uids.append(target.data->image->id.session_uid);
  }
  return true;
}

bool SculptImageBackend::targets_alive() const
{
  if (data_->bmain == nullptr) {
    return true;
  }
  for (const int i : data_->targets.index_range()) {
    const ID *found = BKE_libblock_find_session_uid(data_->bmain, ID_IM, data_->target_uids[i]);
    if (found != &data_->targets[i].data->image->id) {
      return false;
    }
  }
  return true;
}

void SculptImageBackend::restore_backups(const bool free_buffers)
{
  /* A target freed by an undo cannot be restored; the buffers are still released below. */
  const bool alive = this->targets_alive();
  for (PreviewTileBackup &backup : data_->backups) {
    if (backup.orig == nullptr) {
      continue;
    }
    if (alive && backup.target_index >= 0 && backup.target_index < data_->targets.size()) {
      const ShapeImageTarget &target = data_->targets[backup.target_index];
      if (target.data != nullptr) {
        if (free_buffers) {
          shape_tile_backup_restore(
              target.data->image, target.data->image_user, backup.tile_number, *backup.orig);
        }
        else if (!BLI_rcti_is_empty(&backup.dirty)) {
          /* A live refresh only undoes what the previous write touched. */
          shape_tile_backup_restore_region(target.data->image,
                                           target.data->image_user,
                                           backup.tile_number,
                                           *backup.orig,
                                           backup.dirty);
          backup.bleed_pending = true;
        }
      }
    }
    if (free_buffers) {
      IMB_freeImBuf(backup.orig);
    }
  }
  if (free_buffers) {
    data_->backups.clear();
  }
}

void SculptImageBackend::restore_pending_tiles()
{
  if (!this->targets_alive()) {
    return;
  }
  for (PreviewTileBackup &backup : data_->backups) {
    if (!backup.bleed_pending || backup.orig == nullptr || backup.target_index < 0 ||
        backup.target_index >= data_->targets.size())
    {
      continue;
    }
    const ShapeImageTarget &target = data_->targets[backup.target_index];
    if (target.data != nullptr) {
      /* The write pass could not re-derive this tile's seam pixels (no buffer, or it bailed out
       * early): restore the whole tile, which is always consistent. */
      shape_tile_backup_restore(
          target.data->image, target.data->image_user, backup.tile_number, *backup.orig);
    }
    backup.dirty = {0, 0, 0, 0};
    backup.bleed_pending = false;
  }
}

bool SculptImageBackend::preview(bContext *C,
                                 ReportList *reports,
                                 const Span<PaintShape> shapes,
                                 const ShapeStyle &style)
{
  const Depsgraph *depsgraph = ctx_.depsgraph_get(C);
  if (data_->targets.is_empty() || shapes.is_empty() || depsgraph == nullptr ||
      !this->targets_alive())
  {
    return false;
  }
  /* Restore the pixels the previous preview wrote (no free: the backups are reused), then write
   * the new shape; tiles not yet backed up are backed up before their first write. */
  this->restore_backups(false);
  const bool written = shape_write_image(C,
                                         reports,
                                         "Paint Shape",
                                         *ctx_.ob,
                                         *depsgraph,
                                         *ctx_.toolsettings,
                                         *ctx_.space,
                                         *ctx_.paint,
                                         data_->targets,
                                         shapes,
                                         style,
                                         true,
                                         &data_->backups);
  this->restore_pending_tiles();
  if (written) {
    this->tag_preview(C, false);
  }
  return written;
}

bool SculptImageBackend::commit(bContext *C,
                                ReportList *reports,
                                const Span<PaintShape> shapes,
                                const ShapeStyle &style,
                                const char *undo_name)
{
  /* Restore the pre-session pixels and drop the backups before the undo-captured bake, so a
   * bail-out leaves no preview trace and the bake is not applied twice. */
  this->restore_backups(true);
  if (C == nullptr) {
    return false;
  }
  const Depsgraph *depsgraph = ctx_.depsgraph_get(C);
  if (depsgraph == nullptr || !this->targets_alive()) {
    return false;
  }
  return shape_write_image(C,
                           reports,
                           undo_name,
                           *ctx_.ob,
                           *depsgraph,
                           *ctx_.toolsettings,
                           *ctx_.space,
                           *ctx_.paint,
                           data_->targets,
                           shapes,
                           style,
                           false,
                           nullptr);
}

void SculptImageBackend::cancel()
{
  this->restore_backups(true);
}

void SculptImageBackend::preview_force_update(bContext *C)
{
  this->tag_preview(C, true);
}

void SculptImageBackend::tag_preview(bContext *C, const bool force)
{
  /* \a C is null for a context-less refresh: the pixels are updated but nothing is tagged. */
  const double now = BLI_time_now_seconds();
  if (!force && now - data_->last_tag < SHAPE_PREVIEW_SHADING_TAG_INTERVAL) {
    return;
  }
  data_->last_tag = now;
  for (const ShapeImageTarget &target : data_->targets) {
    if (target.data != nullptr && target.data->image != nullptr) {
      if (C != nullptr) {
        WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, target.data->image);
      }
      DEG_id_tag_update(&target.data->image->id, ID_RECALC_SHADING | ID_RECALC_PARAMETERS);
    }
  }
  if (ctx_.ob != nullptr) {
    DEG_id_tag_update(&ctx_.ob->id, ID_RECALC_SHADING);
    if (C != nullptr) {
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ctx_.ob);
    }
  }
}

}  // namespace blender::ed::sculpt_paint::shape
