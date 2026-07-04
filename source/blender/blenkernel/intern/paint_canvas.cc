/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "DNA_brush_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_image.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"

#include "IMB_imbuf_types.hh"

#include <sstream>

namespace blender {

namespace bke::paint::canvas {
static TexPaintSlot *get_active_slot(Object *ob)
{
  Material *mat = BKE_object_material_get(ob, ob->actcol);
  if (mat == nullptr) {
    return nullptr;
  }
  if (mat->texpaintslot == nullptr) {
    return nullptr;
  }
  if (mat->paint_active_slot >= mat->tot_slots) {
    return nullptr;
  }

  TexPaintSlot *slot = &mat->texpaintslot[mat->paint_active_slot];
  return slot;
}

}  // namespace bke::paint::canvas

using namespace blender::bke::paint::canvas;

bool BKE_paint_canvas_image_get(PaintModeSettings *settings,
                                Object *ob,
                                Image **r_image,
                                ImageUser **r_image_user)
{
  *r_image = nullptr;
  *r_image_user = nullptr;

  switch (settings->canvas_source) {
    case PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE:
      break;

    case PAINT_CANVAS_SOURCE_MATERIAL_PAINT:
      /* Vertex float attributes; no single canvas image. */
      break;

    case PAINT_CANVAS_SOURCE_IMAGE:
      *r_image = settings->canvas_image;
      *r_image_user = &settings->image_user;
      break;

    case PAINT_CANVAS_SOURCE_MATERIAL: {
      /* Legacy single-slot lookup (Object Mode project paint / sync).
       * Sculpt Mode=`Material` multi-channel uses
       * #BKE_paint_material_image_targets_get instead. */
      TexPaintSlot *slot = get_active_slot(ob);
      if (slot == nullptr) {
        break;
      }

      *r_image = slot->ima;
      *r_image_user = slot->image_user;
      break;
    }
  }
  return *r_image != nullptr;
}

static bool has_uv_map_attribute(const Mesh &mesh, const StringRef name)
{
  return bke::mesh::is_uv_map(mesh.attributes().lookup_meta_data(name));
}

std::optional<StringRef> BKE_paint_canvas_uvmap_name_get(const PaintModeSettings *settings,
                                                         Object *ob)
{
  switch (settings->canvas_source) {
    case PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE:
      return std::nullopt;
    case PAINT_CANVAS_SOURCE_MATERIAL_PAINT:
      return std::nullopt;
    case PAINT_CANVAS_SOURCE_IMAGE: {
      /* Use active uv map of the object. */
      if (ob->type != OB_MESH) {
        return std::nullopt;
      }

      const Mesh *mesh = id_cast<Mesh *>(ob->data);
      if (!has_uv_map_attribute(*mesh, mesh->active_uv_map_name())) {
        return std::nullopt;
      }
      return mesh->active_uv_map_name();
    }
    case PAINT_CANVAS_SOURCE_MATERIAL: {
      /* Prefer active UV for Principled multi-channel paint; fall back to
       * active texture-paint slot UV when present (legacy project-paint path). */
      if (ob->type != OB_MESH) {
        return std::nullopt;
      }

      const Mesh *mesh = id_cast<Mesh *>(ob->data);
      if (has_uv_map_attribute(*mesh, mesh->active_uv_map_name())) {
        return mesh->active_uv_map_name();
      }

      TexPaintSlot *slot = get_active_slot(ob);
      if (slot == nullptr || slot->uvname == nullptr) {
        return std::nullopt;
      }

      if (!has_uv_map_attribute(*mesh, slot->uvname)) {
        return std::nullopt;
      }
      return slot->uvname;
    }
  }
  return std::nullopt;
}

static void append_image_key(std::stringstream &ss, Image &image, ImageUser &image_user)
{
  ss << ",SEAM_MARGIN:" << image.seam_margin;
  ImageUser tile_user = image_user;
  for (ImageTile &image_tile : image.tiles) {
    tile_user.tile = image_tile.tile_number;
    ImBuf *image_buffer = BKE_image_acquire_ibuf(&image, &tile_user, nullptr);
    if (!image_buffer) {
      continue;
    }
    ss << ",TILE_" << image_tile.tile_number;
    ss << "(" << image_buffer->x << "," << image_buffer->y << ")";
    BKE_image_release_ibuf(&image, image_buffer, nullptr);
  }
}

std::string BKE_paint_pixels_layout_key_get(Image &image,
                                            ImageUser &image_user,
                                            const StringRef uv_map_name)
{
  std::stringstream ss;
  ss << "UV_MAP:" << uv_map_name;
  append_image_key(ss, image, image_user);
  return ss.str();
}

std::string BKE_paint_canvas_key_get(PaintModeSettings *settings, Object *ob, const Brush *brush)
{
  std::stringstream ss;
  ss << "UV_MAP:" << BKE_paint_canvas_uvmap_name_get(settings, ob).value_or("");

  if (settings->canvas_source == PAINT_CANVAS_SOURCE_MATERIAL) {
    /* Key all Principled channel maps so PBVH pixels rebuild when any target changes. */
    const BrushMaterialPaint *brush_paint = brush ? brush->material_paint : nullptr;
    const Vector<PaintMaterialImageTarget> targets = BKE_paint_material_image_targets_get(
        *ob, *settings, brush_paint);
    for (const PaintMaterialImageTarget &target : targets) {
      ss << ",CH" << int(target.channel);
      append_image_key(ss, *target.image, *target.iuser);
    }
    return ss.str();
  }

  Image *image;
  ImageUser *image_user;
  if (BKE_paint_canvas_image_get(settings, ob, &image, &image_user)) {
    append_image_key(ss, *image, *image_user);
  }

  return ss.str();
}

}  // namespace blender
