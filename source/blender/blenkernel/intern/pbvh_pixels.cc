/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_image.hh"
#include "BKE_mesh.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_bvh_pixels.hh"

#include "DNA_image_types.h"
#include "DNA_object_types.h"

#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_set.hh"

#include <unordered_map>
#include <optional>

#include "BKE_image_wrappers.hh"
#include "BKE_paint.hh"

#include "pbvh_intern.hh"
#include "pbvh_pixels_copy.hh"
#include "pbvh_uv_islands.hh"

namespace blender {

namespace bke::pbvh::pixels {

/**
 * Calculate the delta of two neighbor UV coordinates in the given image buffer.
 */
static float2 calc_barycentric_delta(const float2 uvs[3],
                                     const float2 start_uv,
                                     const float2 end_uv)
{

  float3 start_barycentric;
  barycentric_weights_v2(uvs[0], uvs[1], uvs[2], start_uv, start_barycentric);
  float3 end_barycentric;
  barycentric_weights_v2(uvs[0], uvs[1], uvs[2], end_uv, end_barycentric);
  float3 barycentric = end_barycentric - start_barycentric;
  return float2(barycentric.x, barycentric.y);
}

static float2 calc_barycentric_delta_x(const ImBuf *image_buffer,
                                       const float2 uvs[3],
                                       const int x,
                                       const int y)
{
  const float2 start_uv(float(x) / image_buffer->x, float(y) / image_buffer->y);
  const float2 end_uv(float(x + 1) / image_buffer->x, float(y) / image_buffer->y);
  return calc_barycentric_delta(uvs, start_uv, end_uv);
}

/**
 * During debugging this check could be enabled.
 * It will write to each image pixel that is covered by the Tree.
 */
constexpr bool USE_WATERTIGHT_CHECK = false;

static void extract_barycentric_pixels(UDIMTilePixels &tile_data,
                                       const ImBuf *image_buffer,
                                       const uv_islands::UVIslandsMask &uv_mask,
                                       const int uv_island_index,
                                       const int uv_primitive_index,
                                       const float2 uvs[3],
                                       const float2 tile_offset,
                                       const int minx,
                                       const int miny,
                                       const int maxx,
                                       const int maxy)
{
  int pixel_rows_count = 0;
  int total_pixels = 0;

  for (int y = miny; y < maxy; y++) {
    bool start_detected = false;
    PackedPixelRow pixel_row;
    pixel_row.uv_primitive_index = uv_primitive_index;
    pixel_row.num_pixels = 0;
    int x;

    for (x = minx; x < maxx; x++) {
      float2 uv((float(x) + 0.5f) / image_buffer->x, (float(y) + 0.5f) / image_buffer->y);
      float3 barycentric_weights;
      barycentric_weights_v2(uvs[0], uvs[1], uvs[2], uv, barycentric_weights);

      const bool is_inside = barycentric_inside_triangle_v2(barycentric_weights);
      const bool is_masked = uv_mask.is_masked(uv_island_index, uv + tile_offset);
      if (!start_detected && is_inside && is_masked) {
        start_detected = true;
        pixel_row.start_image_coordinate = ushort2(x, y);
        pixel_row.start_barycentric_coord = float2(barycentric_weights.x, barycentric_weights.y);
      }
      else if (start_detected && (!is_inside || !is_masked)) {
        break;
      }
    }

    if (!start_detected) {
      continue;
    }
    pixel_row.num_pixels = x - pixel_row.start_image_coordinate.x;
    tile_data.pixel_rows.append(pixel_row);
    pixel_rows_count++;
    total_pixels += pixel_row.num_pixels;
  }

  if (pixel_rows_count > 0) {
    printf("[DEBUG PIXELS] uv_prim=%d, island=%d: extracted %d pixel rows, %d total pixels "
           "(bounds: x[%d,%d] y[%d,%d])\n",
           uv_primitive_index, uv_island_index, pixel_rows_count, total_pixels,
           minx, maxx, miny, maxy);
  }
}

/** Update the geometry primitives of the pbvh. */
static void update_geom_primitives(Tree &pbvh, const uv_islands::MeshData &mesh_data)
{
  PBVHData &pbvh_data = data_get(pbvh);
  pbvh_data.vert_tris.reinitialize(mesh_data.corner_tris.size());
  bke::mesh::vert_tris_from_corner_tris(
      mesh_data.corner_verts, mesh_data.corner_tris, pbvh_data.vert_tris);
}

struct UVPrimitiveLookup {
  struct Entry {
    uv_islands::UVPrimitive *uv_primitive;
    uint64_t uv_island_index;

    Entry(uv_islands::UVPrimitive *uv_primitive, uint64_t uv_island_index)
        : uv_primitive(uv_primitive), uv_island_index(uv_island_index)
    {
    }
  };

  Vector<Vector<Entry>> lookup;

  UVPrimitiveLookup(const uint64_t geom_primitive_len, uv_islands::UVIslands &uv_islands)
  {
    lookup.append_n_times(Vector<Entry>(), geom_primitive_len);

    uint64_t uv_island_index = 0;
    for (uv_islands::UVIsland &uv_island : uv_islands.islands) {
      for (uv_islands::UVPrimitive &uv_primitive : uv_island.uv_primitives) {
        lookup[uv_primitive.primitive_i].append_as(Entry(&uv_primitive, uv_island_index));
      }
      uv_island_index++;
    }

    /* DEBUG: Print lookup statistics */
    printf("[DEBUG LOOKUP] UVPrimitiveLookup created:\n");
    int total_entries = 0;
    int max_entries = 0;
    int tri_with_multiple = 0;
    for (uint64_t tri = 0; tri < geom_primitive_len; tri++) {
      int count = lookup[tri].size();
      total_entries += count;
      if (count > 1) {
        tri_with_multiple++;
        printf("[DEBUG LOOKUP]   tri=%d has %d UV primitives:\n", (int)tri, count);
        for (int i = 0; i < count; i++) {
          printf("[DEBUG LOOKUP]     [%d] uv_prim=%p, island=%d\n",
                 i, lookup[tri][i].uv_primitive, (int)lookup[tri][i].uv_island_index);
        }
      }
      if (count > max_entries) {
        max_entries = count;
      }
    }
    printf("[DEBUG LOOKUP] Total: %d entries, %d/%d tris have multiple entries, max per tri: %d\n",
           total_entries, tri_with_multiple, (int)geom_primitive_len, max_entries);
  }
};

static void do_encode_pixels(const uv_islands::MeshData &mesh_data,
                             const uv_islands::UVIslandsMask &uv_masks,
                             const UVPrimitiveLookup &uv_prim_lookup,
                             Image &image,
                             ImageUser &image_user,
                             MeshNode &node,
                             const std::optional<float2> &brush_pos_ss = std::nullopt,
                             const float brush_radius_ss = 0.0f)
{
  NodeData *node_data = static_cast<NodeData *>(node.pixels_);

  const bool use_brush_filter = brush_pos_ss.has_value() && brush_radius_ss > 0.0f;
  
  printf("[DEBUG ENCODE] Starting encode for node with %d faces, brush_filter=%s\n", 
         (int)node.faces().size(), use_brush_filter ? "YES" : "NO");
  if (use_brush_filter) {
    printf("[DEBUG ENCODE]   brush_pos_ss=[%.1f,%.1f], brush_radius_ss=%.1f\n",
           brush_pos_ss->x, brush_pos_ss->y, brush_radius_ss);
  }

  int total_uv_primitives = 0;
  int duplicates_skipped = 0;
  int brush_filter_skipped = 0;
  int target_island_skipped = 0;
  int total_tris_processed = 0;

  /* Structure to track processed UV primitives by their UV coordinates */
  struct UVKey {
    float2 uvs[3];

    UVKey(const float2 uvs_[3])
    {
      uvs[0] = uvs_[0];
      uvs[1] = uvs_[1];
      uvs[2] = uvs_[2];
    }

    bool operator==(const UVKey &other) const
    {
      return uvs[0] == other.uvs[0] && uvs[1] == other.uvs[1] && uvs[2] == other.uvs[2];
    }
  };

  struct UVKeyHash {
    uint64_t operator()(const UVKey &key) const
    {
      /* Simple hash of UV coordinates */
      uint64_t h = 0;
      for (int i = 0; i < 3; i++) {
        uint32_t ux = *reinterpret_cast<const uint32_t *>(&key.uvs[i].x);
        uint32_t uy = *reinterpret_cast<const uint32_t *>(&key.uvs[i].y);
        h ^= (static_cast<uint64_t>(ux) << 32) | uy;
      }
      return h;
    }
  };

  for (ImageTile &tile : image.tiles) {
    image::ImageTileWrapper image_tile(&tile);
    image_user.tile = image_tile.get_tile_number();
    ImBuf *image_buffer = BKE_image_acquire_ibuf(&image, &image_user, nullptr);
    if (image_buffer == nullptr) {
      continue;
    }

    UDIMTilePixels tile_data;
    tile_data.tile_number = image_tile.get_tile_number();
    float2 tile_offset = float2(image_tile.get_tile_offset());

    /* Track processed UV primitives by their UV coordinates to avoid duplicates.
     * Using std::unordered_map to detect UV primitives with same coordinates. */
    std::unordered_map<UVKey, uintptr_t, UVKeyHash> processed_uv_keys;

    /* Track which geometry tris have been processed for this tile */
    Set<int> processed_tris;

    for (const int face : node.faces()) {
      printf("[DEBUG FACES] Processing face=%d\n", face);
      
      /* Get the UV island ID for this face's first triangle to filter UV primitives */
      int first_tri = -1;
      for (const int tri : bke::mesh::face_triangles_range(mesh_data.faces, face)) {
        first_tri = tri;
        break;
      }
      if (first_tri == -1) {
        continue;
      }
      
      /* Get the island index from the first UV primitive in the lookup */
        const int target_island_index = (uv_prim_lookup.lookup[first_tri].size() > 0) 
            ? uv_prim_lookup.lookup[first_tri][0].uv_island_index 
            : -1;
        printf("[DEBUG FACES]   face=%d, first_tri=%d, target_island=%d\n", face, first_tri, target_island_index);
      printf("[DEBUG FACES]   face=%d, first_tri=%d, target_island=%d\n", face, first_tri, target_island_index);
      
      for (const int tri : bke::mesh::face_triangles_range(mesh_data.faces, face)) {
        total_tris_processed++;
        bool tri_already_processed = processed_tris.contains(tri);
        if (tri_already_processed) {
          printf("[DEBUG FACES]   tri=%d ALREADY PROCESSED in this tile (duplicate from different face!)\n", tri);
        }
        processed_tris.add_new(tri);

        printf("[DEBUG FACES]   tri=%d, lookup has %d UV primitives\n", tri, (int)uv_prim_lookup.lookup[tri].size());
        
        /* Only process UV primitives that belong to the same island as this face */
        for (const UVPrimitiveLookup::Entry &entry : uv_prim_lookup.lookup[tri]) {
          /* Skip UV primitives from different islands - this prevents projection spreading */
          if (entry.uv_island_index != target_island_index) {
            target_island_skipped++;
            printf("[DEBUG ISLAND] tri=%d SKIP uv_prim=%d island=%d target=%d\n",
                   tri,
                   entry.uv_primitive->primitive_i,
                   (int)entry.uv_island_index,
                   target_island_index);
            continue;
          }
          /* Get UV coordinates BEFORE tile offset for deduplication */
          float2 uvs_raw[3] = {
              entry.uv_primitive->get_uv_vertex(mesh_data, 0)->uv,
              entry.uv_primitive->get_uv_vertex(mesh_data, 1)->uv,
              entry.uv_primitive->get_uv_vertex(mesh_data, 2)->uv,
          };

          /* Deduplication: Skip if we've already processed UV primitives with these exact coordinates */
          UVKey uv_key(uvs_raw);
          if (processed_uv_keys.contains(uv_key)) {
            duplicates_skipped++;
            printf("[DEBUG UV PROJECTION] SKIP DUPLICATE: tri=%d, UVs=[%.3f,%.3f][%.3f,%.3f][%.3f,%.3f] (duplicate UV coordinates)\n",
                   tri, uvs_raw[0].x, uvs_raw[0].y, uvs_raw[1].x, uvs_raw[1].y,
                   uvs_raw[2].x, uvs_raw[2].y);
            continue;
          }
          uintptr_t uv_prim_ptr = reinterpret_cast<uintptr_t>(entry.uv_primitive);
          processed_uv_keys.emplace(uv_key, uv_prim_ptr);

          /* Apply tile offset for actual pixel extraction */
          float2 uvs[3] = {
              uvs_raw[0] - tile_offset,
              uvs_raw[1] - tile_offset,
              uvs_raw[2] - tile_offset,
          };

          const float minv = clamp_f(min_fff(uvs[0].y, uvs[1].y, uvs[2].y), 0.0f, 1.0f);
          const int miny = floor(minv * image_buffer->y);
          const float maxv = clamp_f(max_fff(uvs[0].y, uvs[1].y, uvs[2].y), 0.0f, 1.0f);
          const int maxy = min_ii(ceil(maxv * image_buffer->y), image_buffer->y);
          const float minu = clamp_f(min_fff(uvs[0].x, uvs[1].x, uvs[2].x), 0.0f, 1.0f);
          const int minx = floor(minu * image_buffer->x);
          const float maxu = clamp_f(max_fff(uvs[0].x, uvs[1].x, uvs[2].x), 0.0f, 1.0f);
          const int maxx = min_ii(ceil(maxu * image_buffer->x), image_buffer->x);

          /* Brush position filtering: skip UV primitives that are far from brush position.
           * Convert UV bounds to screen-space and check intersection with brush radius. */
          bool should_skip = false;
          
          if (use_brush_filter) {
            /* Convert UV bounds (image pixel coords) to screen-space approx */
            /* minx,miny and maxx,maxy are in image pixel coordinates (without tile offset) */
            /* We need to check if this area is within brush radius of brush_pos_ss */
            
            /* Calculate center of UV primitive in screen space */
            float center_ss_x = float(minx + maxx) * 0.5f;
            float center_ss_y = float(miny + maxy) * 0.5f;
            
            /* Calculate approximate distance from brush to UV primitive center */
            float dist_x = center_ss_x - brush_pos_ss->x;
            float dist_y = center_ss_y - brush_pos_ss->y;
            float dist = sqrtf(dist_x * dist_x + dist_y * dist_y);
            
            /* Calculate radius of UV primitive */
            float uv_radius = sqrtf(float(maxx - minx) * float(maxx - minx) + 
                                   float(maxy - miny) * float(maxy - miny)) * 0.5f;
            
            /* Skip if UV primitive is completely outside brush influence */
            if (dist > brush_radius_ss + uv_radius) {
              should_skip = true;
              brush_filter_skipped++;
              printf("[DEBUG BRUSH FILTER] SKIP: tri=%d, dist=%.1f > brush_radius=%.1f + uv_radius=%.1f\n",
                     tri, dist, brush_radius_ss, uv_radius);
            }
            else {
              printf("[DEBUG BRUSH FILTER] KEEP tri=%d, dist=%.1f <= brush_radius=%.1f + uv_radius=%.1f, bounds x[%d,%d] y[%d,%d]\n",
                     tri, dist, brush_radius_ss, uv_radius, minx, maxx, miny, maxy);
            }
          }
          else {
            /* Fallback: filter by distance from image center.
             * This helps when brush position is not available.
             * Skip UV primitives that are far from the center of the image. */
            float center_x = float(minx + maxx) * 0.5f;
            float center_y = float(miny + maxy) * 0.5f;
            float image_center_x = float(image_buffer->x) * 0.5f;
            float image_center_y = float(image_buffer->y) * 0.5f;
            
            float dist_x = center_x - image_center_x;
            float dist_y = center_y - image_center_y;
            float dist = sqrtf(dist_x * dist_x + dist_y * dist_y);
            
            /* Skip if UV primitive is more than 40% of image size from center */
            float max_dist = sqrtf(image_center_x * image_center_x + image_center_y * image_center_y) * 0.4f;
             if (dist > max_dist) {
              should_skip = true;
              brush_filter_skipped++;
              printf("[DEBUG CENTER FILTER] SKIP: tri=%d, dist=%.1f > max_dist=%.1f (center based)\n",
                     tri, dist, max_dist);
            }
          }
          
          if (should_skip) {
            continue;
          }

          /* Log UV projection details */
          printf("[DEBUG UV PROJECTION] tri=%d, island=%d, UVs: "
                 "[%.3f,%.3f] [%.3f,%.3f] [%.3f,%.3f], tile_offset=[%.1f,%.1f], "
                 "bounds: x[%d,%d] y[%d,%d]\n",
                 tri, (int)entry.uv_island_index,
                 uvs[0].x, uvs[0].y, uvs[1].x, uvs[1].y, uvs[2].x, uvs[2].y,
                 tile_offset.x, tile_offset.y,
                 minx, maxx, miny, maxy);

          total_uv_primitives++;

          /* TODO: Perform bounds check */
          int uv_prim_index = node_data->uv_primitives.size();
          node_data->uv_primitives.append(tri);
          UVPrimitivePaintInput &paint_input = node_data->uv_primitives.last();

          /* Calculate barycentric delta */
          paint_input.delta_barycentric_coord_u = calc_barycentric_delta_x(
              image_buffer, uvs, minx, miny);

          /* DEBUG: Print detailed info about each UV primitive being processed */
          printf("[DEBUG EXTRACT] face=%d tri=%d uv_prim=%d island=%d, "
                 "UVs=[%.3f,%.3f][%.3f,%.3f][%.3f,%.3f], "
                 "bounds: x[%d,%d] y[%d,%d], pixels=%d\n",
                 face, tri, uv_prim_index, (int)entry.uv_island_index,
                 uvs[0].x, uvs[0].y, uvs[1].x, uvs[1].y, uvs[2].x, uvs[2].y,
                 minx, maxx, miny, maxy, (maxx - minx) * (maxy - miny));

          /* Extract the pixels. */
          extract_barycentric_pixels(tile_data,
                                     image_buffer,
                                     uv_masks,
                                     entry.uv_island_index,
                                     uv_prim_index,
                                     uvs,
                                     tile_offset,
                                     minx,
                                     miny,
                                     maxx,
                                     maxy);
        }
      }
    }
    BKE_image_release_ibuf(&image, image_buffer, nullptr);

    if (tile_data.pixel_rows.is_empty()) {
      continue;
    }

    node_data->tiles.append(tile_data);
  }

  printf("[DEBUG ENCODE] Finished encode - processed %d UV primitives, %d tiles, %d duplicates skipped, %d brush_filter_skipped, %d target_island_skipped, %d tri iterations\n",
         total_uv_primitives,
         (int)node_data->tiles.size(),
         duplicates_skipped,
         brush_filter_skipped,
         target_island_skipped,
         total_tris_processed);
}

static bool should_pixels_be_updated(const Node &node)
{
  if ((node.flag_ & (Node::Leaf | Node::TexLeaf)) == 0) {
    return false;
  }
  if (node.children_offset_ != 0) {
    return false;
  }
  if ((node.flag_ & Node::RebuildPixels) != 0) {
    return true;
  }
  NodeData *node_data = static_cast<NodeData *>(node.pixels_);
  if (node_data != nullptr) {
    return false;
  }
  return true;
}

static int count_nodes_to_update(Tree &pbvh)
{
  int result = 0;
  for (Node &node : pbvh.nodes<MeshNode>()) {
    if (should_pixels_be_updated(node)) {
      result++;
    }
  }
  return result;
}

/**
 * Find the nodes that needs to be updated.
 *
 * The nodes that require updated are added to the r_nodes_to_update parameter.
 * Will fill in r_visited_polygons with polygons that are owned by nodes that do not require
 * updates.
 *
 * returns if there were any nodes found (true).
 */
static bool find_nodes_to_update(Tree &pbvh, Vector<MeshNode *> &r_nodes_to_update)
{
  int nodes_to_update_len = count_nodes_to_update(pbvh);
  if (nodes_to_update_len == 0) {
    return false;
  }

  /* Init or reset Tree pixel data when changes detected. */
  if (pbvh.pixels_ == nullptr) {
    PBVHData *pbvh_data = MEM_new<PBVHData>(__func__);
    pbvh.pixels_ = pbvh_data;
  }
  else {
    PBVHData *pbvh_data = static_cast<PBVHData *>(pbvh.pixels_);
    pbvh_data->clear_data();
  }

  r_nodes_to_update.reserve(nodes_to_update_len);

  for (MeshNode &node : pbvh.nodes<MeshNode>()) {
    if (!should_pixels_be_updated(node)) {
      continue;
    }
    r_nodes_to_update.append(&node);
    node.flag_ = (node.flag_ | Node::RebuildPixels);

    if (node.pixels_ == nullptr) {
      NodeData *node_data = MEM_new<NodeData>(__func__);
      node.pixels_ = node_data;
    }
    else {
      NodeData *node_data = static_cast<NodeData *>(node.pixels_);
      node_data->clear_data();
    }
  }

  return true;
}

static void apply_watertight_check(Tree &pbvh, Image &image, ImageUser &image_user)
{
  ImageUser watertight = image_user;
  for (ImageTile &tile_data : image.tiles) {
    image::ImageTileWrapper image_tile(&tile_data);
    watertight.tile = image_tile.get_tile_number();
    ImBuf *image_buffer = BKE_image_acquire_ibuf(&image, &watertight, nullptr);
    if (image_buffer == nullptr) {
      continue;
    }
    for (Node &node : pbvh.nodes<MeshNode>()) {
      if ((node.flag_ & Node::Leaf) == 0) {
        continue;
      }
      NodeData *node_data = static_cast<NodeData *>(node.pixels_);
      UDIMTilePixels *tile_node_data = node_data->find_tile_data(image_tile);
      if (tile_node_data == nullptr) {
        continue;
      }

      for (PackedPixelRow &pixel_row : tile_node_data->pixel_rows) {
        int pixel_offset = pixel_row.start_image_coordinate.y * image_buffer->x +
                           pixel_row.start_image_coordinate.x;
        for (int x = 0; x < pixel_row.num_pixels; x++) {
          if (image_buffer->float_buffer.data) {
            copy_v4_fl(&image_buffer->float_buffer.data[pixel_offset * 4], 1.0);
          }
          if (image_buffer->byte_buffer.data) {
            uint8_t *dest = &image_buffer->byte_buffer.data[pixel_offset * 4];
            dest[0] = dest[1] = dest[2] = dest[3] = 255;
          }
          pixel_offset += 1;
        }
      }
    }
    BKE_image_release_ibuf(&image, image_buffer, nullptr);
  }
  BKE_image_partial_update_mark_full_update(&image);
}

static bool update_pixels(const Depsgraph &depsgraph,
                          const Object &object,
                          Tree &pbvh,
                          Image &image,
                          ImageUser &image_user,
                          const std::optional<float2> &brush_pos_ss = std::nullopt,
                          const float brush_radius_ss = 0.0f)
{
  Vector<MeshNode *> nodes_to_update;
  if (!find_nodes_to_update(pbvh, nodes_to_update)) {
    return false;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const StringRef active_uv_name = mesh.active_uv_map_name();
  if (active_uv_name.is_empty()) {
    return false;
  }

  const AttributeAccessor attributes = mesh.attributes();
  const VArraySpan uv_map = *attributes.lookup<float2>(active_uv_name, AttrDomain::Corner);

  uv_islands::MeshData mesh_data(mesh.faces(),
                                 mesh.corner_tris(),
                                 mesh.corner_verts(),
                                 uv_map,
                                 bke::pbvh::vert_positions_eval(depsgraph, object));

  /* DEBUG: Print mesh data info */
  printf("[DEBUG] UV Paint: mesh has %d corner_tris, %d corners\n",
         (int)mesh_data.corner_tris.size(), (int)mesh_data.corner_verts.size());
  printf("[DEBUG] UV Paint: UV map has %d elements\n", (int)mesh_data.uv_map.size());

  /* UV island caching is disabled due to projection issues with stroke duplication.
   * TODO: investigate and re-enable with proper invalidation */
#if 0
  Array<int> cached_island_ids;
  int64_t cached_island_count = 0;
  const bool has_cache = uv_islands::UVIslandCache::get_cached_island_ids(
      mesh_data, cached_island_ids, cached_island_count);
  if (has_cache && cached_island_ids.size() == mesh_data.corner_tris.size()) {
    mesh_data.uv_island_ids = cached_island_ids;
    mesh_data.uv_island_len = cached_island_count;
    printf("[DEBUG] Using cached island IDs: %d islands\n", (int)cached_island_count);
  }
#endif

  /* Create UV islands */
  uv_islands::UVIslands islands(mesh_data);
  printf("[DEBUG] Created %d UV islands\n", (int)islands.islands.size());

#if 0
  /* Store island IDs in cache for next time */
  if (!has_cache) {
    uv_islands::UVIslandCache::store_island_ids(
        mesh_data, mesh_data.uv_island_ids, mesh_data.uv_island_len);
  }
#endif

  uv_islands::UVIslandsMask uv_masks;
  ImageUser tile_user = image_user;
  for (ImageTile &tile_data : image.tiles) {
    image::ImageTileWrapper image_tile(&tile_data);
    tile_user.tile = image_tile.get_tile_number();
    ImBuf *tile_buffer = BKE_image_acquire_ibuf(&image, &tile_user, nullptr);
    if (tile_buffer == nullptr) {
      continue;
    }
    uv_masks.add_tile(float2(image_tile.get_tile_x_offset(), image_tile.get_tile_y_offset()),
                      ushort2(tile_buffer->x, tile_buffer->y));
    BKE_image_release_ibuf(&image, tile_buffer, nullptr);
  }
  uv_masks.add(mesh_data, islands);
  uv_masks.dilate(image.seam_margin);

  islands.extract_borders();
  islands.extend_borders(mesh_data, uv_masks);
  update_geom_primitives(pbvh, mesh_data);

  UVPrimitiveLookup uv_primitive_lookup(mesh_data.corner_tris.size(), islands);

  threading::parallel_for(nodes_to_update.index_range(), 1, [&](const IndexRange range) {
    for (const int i : range) {
      do_encode_pixels(
          mesh_data, uv_masks, uv_primitive_lookup, image, image_user, *nodes_to_update[i],
          brush_pos_ss, brush_radius_ss);
    }
  });
  if (USE_WATERTIGHT_CHECK) {
    apply_watertight_check(pbvh, image, image_user);
  }

  /* Add solution for non-manifold parts of the model. */
  copy_update(pbvh, image, image_user, mesh_data);

  /* Rebuild the undo regions. */
  for (Node *node : nodes_to_update) {
    NodeData *node_data = static_cast<NodeData *>(node->pixels_);
    node_data->rebuild_undo_regions();
  }

  /* Clear the UpdatePixels flag. */
  for (Node *node : nodes_to_update) {
    node->flag_ &= ~Node::RebuildPixels;
  }

  /* Add Node::TexLeaf flag */
  for (Node &node : pbvh.nodes<MeshNode>()) {
    if (node.flag_ & Node::Leaf) {
      node.flag_ |= Node::TexLeaf;
    }
  }

  printf("[DEBUG] UV Paint update complete - %d nodes updated, %d UV islands processed\n",
         (int)nodes_to_update.size(), (int)islands.islands.size());

// #define DO_PRINT_STATISTICS
#ifdef DO_PRINT_STATISTICS
  /* Print some statistics about compression ratio. */
  {
    int compressed_data_len = 0;
    int num_pixels = 0;
    for (int n = 0; n < pbvh->totnode; n++) {
      Node *node = &pbvh->nodes[n];
      if ((node->flag & Node::Leaf) == 0) {
        continue;
      }
      NodeData *node_data = static_cast<NodeData *>(node->pixels.node_data);
      for (const UDIMTilePixels &tile_data : node_data->tiles) {
        compressed_data_len += tile_data.encoded_pixels.size() * sizeof(PackedPixelRow);
        for (const PackedPixelRow &encoded_pixels : tile_data.encoded_pixels) {
          num_pixels += encoded_pixels.num_pixels;
        }
      }
    }
    printf("Encoded %lld pixels in %lld bytes (%f bytes per pixel)\n",
           num_pixels,
           compressed_data_len,
           float(compressed_data_len) / num_pixels);
  }
#endif

  return true;
}

NodeData &node_data_get(Node &node)
{
  BLI_assert(node.pixels_ != nullptr);
  NodeData *node_data = static_cast<NodeData *>(node.pixels_);
  return *node_data;
}

PBVHData &data_get(Tree &pbvh)
{
  BLI_assert(pbvh.pixels_ != nullptr);
  PBVHData *data = static_cast<PBVHData *>(pbvh.pixels_);
  return *data;
}

void mark_image_dirty(Node &node, Image &image, ImageUser &image_user)
{
  BLI_assert(node.pixels_ != nullptr);
  NodeData *node_data = static_cast<NodeData *>(node.pixels_);
  if (node_data->flags.dirty) {
    ImageUser local_image_user = image_user;
    for (UDIMTilePixels &tile_data : node_data->tiles) {
      if (!tile_data.flags.dirty) {
        continue;
      }

      ImageTile *image_tile_ptr = BKE_image_get_tile(&image, tile_data.tile_number);
      if (image_tile_ptr == nullptr) {
        continue;
      }

      image::ImageTileWrapper image_tile(image_tile_ptr);
      local_image_user.tile = tile_data.tile_number;
      ImBuf *image_buffer = BKE_image_acquire_ibuf(&image, &local_image_user, nullptr);
      if (image_buffer == nullptr) {
        continue;
      }

      node_data->mark_region(image, image_tile, *image_buffer);
      BKE_image_release_ibuf(&image, image_buffer, nullptr);
    }
    node_data->flags.dirty = false;
  }
}

void collect_dirty_tiles(Node &node, Vector<image::TileNumber> &r_dirty_tiles)
{
  NodeData *node_data = static_cast<NodeData *>(node.pixels_);
  node_data->collect_dirty_tiles(r_dirty_tiles);
}

}  // namespace bke::pbvh::pixels

namespace bke::pbvh {

void build_pixels(const Depsgraph &depsgraph, Object &object, Image &image, ImageUser &image_user,
                 const std::optional<float2> &brush_pos_ss, const float brush_radius_ss)
{
  Tree &pbvh = *object::pbvh_get(object);
  pixels::update_pixels(depsgraph, object, pbvh, image, image_user, brush_pos_ss, brush_radius_ss);
}

void node_pixels_free(Node *node)
{
  pixels::NodeData *node_data = static_cast<pixels::NodeData *>(node->pixels_);

  if (!node_data) {
    return;
  }

  MEM_delete(node_data);
  node->pixels_ = nullptr;
}

void pixels_free(Tree *pbvh)
{
  pixels::PBVHData *pbvh_data = static_cast<pixels::PBVHData *>(pbvh->pixels_);
  MEM_delete(pbvh_data);
  pbvh->pixels_ = nullptr;
}

}  // namespace bke::pbvh
}  // namespace blender
