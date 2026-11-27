/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The raster side of a Text Decal effect: the rasterized styled text of its Text object is the
 * mask of the whole effect, kept in a coverage map the effect owns
 * (#MaterialPaintLayerText::coverage). The effect works like a Fill otherwise: each channel holds
 * a constant value or the texture the user dropped on it, and the generator and the CPU composite
 * scale what the channels lay by the coverage. The raster therefore depends only on the text, its
 * formatting and the bake options; a channel value, colour, texture or blend never reaches it. The
 * CPU renders on the main thread (the BLF font map is global). A mask item has no channel values
 * and keeps its coverage as the grey of its Base Color map. Like a Warp mapping this is parallel
 * to the bake planner: the planner bakes rows, not effects, and the text raster is not a row bake.
 */

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_tile.hh"
#include "BKE_vfont_render.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "BLI_hash_mm2a.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_threads.h"
#include "BLI_time.h"
#include "BLI_timer.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "BKE_curves.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_vector.hh"
#include "BKE_vfont.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DNA_ID.h"
#include "DNA_curve_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_packedFile_types.h"
#include "DNA_paint_vector_types.h"
#include "DNA_vfont_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_layers_bake_intern.hh"
#include "paint_layers_hash_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_text_decal_intern.hh"

namespace blender {

/** Bumped when what a Text Decal raster stores changes meaning, so older caches invalidate once.
 */
constexpr uint64_t TEXT_DECAL_HASH_VERSION = 12;

/**
 * Base em height of a canvas text, in UDIM UV, that a file written before the field existed reads
 * as (the DNA default). Zero must never reach the raster's own fallback, which is a fraction of the
 * whole curve rather than the font size.
 */
constexpr float TEXT_DECAL_BEND_FONT_SIZE_UV_DEFAULT = 0.1f;
/** The RNA range of #MaterialPaintLayerText::bend_font_size_uv. */
constexpr float TEXT_DECAL_BEND_FONT_SIZE_UV_MIN = 0.001f;
constexpr float TEXT_DECAL_BEND_FONT_SIZE_UV_MAX = 1.0f;

/** The effective base em height: zero reads as the default and an out-of-range value is clamped,
 * so the hash and the raster always see the same number. */
static float text_decal_bend_font_size_uv(const MaterialPaintLayerText &payload)
{
  if (!(payload.bend_font_size_uv > 0.0f)) {
    return TEXT_DECAL_BEND_FONT_SIZE_UV_DEFAULT;
  }
  return std::clamp(payload.bend_font_size_uv,
                    TEXT_DECAL_BEND_FONT_SIZE_UV_MIN,
                    TEXT_DECAL_BEND_FONT_SIZE_UV_MAX);
}

/** The bend polyline in the raster's own normalized UV, as the layout reads it. */
struct TextDecalBend {
  Vector<float2> points;
  bool cyclic = false;

  bool is_empty() const
  {
    return points.size() < 2;
  }
};

/** Everything the 3D bend projection needs, resolved once for a whole curve. */
struct TextDecalBendProjector {
  float center[3] = {0.0f, 0.0f, 0.0f};
  float size[3] = {1.0f, 1.0f, 1.0f};
  /** The frame basis: `rot[c]` is the c-th frame axis in the controller's space. */
  float rot[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
  float inv_rot[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
  int8_t axis = MA_PAINT_LAYER_PROJECTION_AXIS_Z;
  int u_index = 0;
  int v_index = 1;
  float aspect[3] = {1.0f, 1.0f, 1.0f};
  bool mapping_remaps = false;
  float map_cos = 1.0f;
  float map_sin = 0.0f;
  float map_scale_x = 1.0f;
  float map_scale_y = 1.0f;
  float map_offset[2] = {0.0f, 0.0f};
  /** The object whose space the frame lives in, or null for the painted object's own space. */
  const Object *controller = nullptr;
};

static bool text_decal_bend_projector_build(const Material &ma,
                                            const MaterialPaintLayer &row,
                                            TextDecalBendProjector &r_projector)
{
  if (row.projection == nullptr) {
    return false;
  }
  const MaterialPaintLayerProjection &projection = *row.projection;
  r_projector.controller = row.projection_object;
  copy_v3_v3(r_projector.center, projection.center);
  BKE_paint_layers_projection_size_effective(row, r_projector.size);
  BKE_paint_layers_projection_frame_matrix(projection.normal, projection.roll, r_projector.rot);
  transpose_m3_m3(r_projector.inv_rot, r_projector.rot);
  r_projector.axis = BKE_paint_layers_projection_axis_get(row);
  r_projector.u_index = (r_projector.axis == MA_PAINT_LAYER_PROJECTION_AXIS_X) ? 1 : 0;
  r_projector.v_index = (r_projector.axis == MA_PAINT_LAYER_PROJECTION_AXIS_Z) ? 1 : 2;
  if (BKE_paint_layers_projection_fit_aspect_get(row)) {
    float factors[2];
    BKE_paint_layers_projection_aspect_factors(
        (row.text_decal != nullptr) ? row.text_decal->coverage : nullptr, factors);
    r_projector.aspect[r_projector.u_index] = factors[0];
    r_projector.aspect[r_projector.v_index] = factors[1];
  }
  r_projector.mapping_remaps = BKE_paint_layers_mapping_remaps(ma, row);
  if (r_projector.mapping_remaps) {
    r_projector.map_cos = std::cos(row.mapping.rotation);
    r_projector.map_sin = std::sin(row.mapping.rotation);
    r_projector.map_scale_x = BKE_paint_layers_mapping_scale_normalize(row.mapping.scale[0]);
    r_projector.map_scale_y = BKE_paint_layers_mapping_scale_normalize(row.mapping.scale[1]);
    r_projector.map_offset[0] = row.mapping.offset[0];
    r_projector.map_offset[1] = row.mapping.offset[1];
  }
  return true;
}

/** The painted object's space -> the decal frame's UV, through every step the map read takes. */
static void text_decal_bend_uv_from_point(const TextDecalBendProjector &projector,
                                          const Object *painted_object,
                                          const float p_in[3],
                                          float r_uv[2])
{
  float p[3] = {p_in[0], p_in[1], p_in[2]};
  if (projector.controller != nullptr && painted_object != nullptr) {
    /* The frame lives in the controller's space, while the curve document is written in the
     * painted object's: carry the point through world space into the controller. */
    float p_world[3];
    mul_v3_m4v3(p_world, painted_object->object_to_world().ptr(), p);
    mul_v3_m4v3(p, projector.controller->world_to_object().ptr(), p_world);
  }
  float q[3];
  BKE_paint_layers_projection_frame_position(
      projector.center, projector.size, projector.inv_rot, p, q);
  for (int a = 0; a < 3; a++) {
    q[a] *= projector.aspect[a];
  }
  BKE_paint_layers_projection_planar_uv(projector.axis, q, r_uv);
  if (projector.mapping_remaps) {
    const float ru = r_uv[0] * projector.map_cos - r_uv[1] * projector.map_sin;
    const float rv = r_uv[0] * projector.map_sin + r_uv[1] * projector.map_cos;
    r_uv[0] = ru * projector.map_scale_x + projector.map_offset[0];
    r_uv[1] = rv * projector.map_scale_y + projector.map_offset[1];
  }
}

/** The inverse of #text_decal_bend_uv_from_point: a raster-UV point back onto the frame plane. */
static void text_decal_bend_point_from_uv(const TextDecalBendProjector &projector,
                                          const Object *painted_object,
                                          const float uv_in[2],
                                          float r_p[3])
{
  float uv[2] = {uv_in[0], uv_in[1]};
  if (projector.mapping_remaps) {
    uv[0] -= projector.map_offset[0];
    uv[1] -= projector.map_offset[1];
    uv[0] /= projector.map_scale_x;
    uv[1] /= projector.map_scale_y;
    /* Inverse rotation: transpose of the forward rotation. */
    const float ru = uv[0] * projector.map_cos + uv[1] * projector.map_sin;
    const float rv = -uv[0] * projector.map_sin + uv[1] * projector.map_cos;
    uv[0] = ru;
    uv[1] = rv;
  }
  float q[3] = {0.0f, 0.0f, 0.0f};
  q[projector.u_index] = 2.0f * uv[0] - 1.0f;
  q[projector.v_index] = 2.0f * uv[1] - 1.0f;
  for (int a = 0; a < 3; a++) {
    q[a] /= projector.aspect[a];
  }
  float p[3];
  for (int a = 0; a < 3; a++) {
    const float half_extent = BKE_paint_layers_projection_size_normalize(projector.size[a]) * 0.5f;
    p[a] = q[a] * half_extent;
  }
  float p_controller[3];
  mul_v3_m3v3(p_controller, projector.rot, p);
  add_v3_v3(p_controller, projector.center);
  if (projector.controller != nullptr && painted_object != nullptr) {
    float p_world[3];
    mul_v3_m4v3(p_world, projector.controller->object_to_world().ptr(), p_controller);
    mul_v3_m4v3(p, painted_object->world_to_object().ptr(), p_world);
  }
  else {
    copy_v3_v3(p, p_controller);
  }
  copy_v3_v3(r_p, p);
}

static bool text_decal_bend_build(const Material &ma,
                                  const MaterialPaintLayer &row,
                                  const Object *painted_object,
                                  TextDecalBend &r_bend)
{
  const MaterialPaintLayerText &payload = *row.text_decal;
  r_bend.points.clear();
  r_bend.cyclic = false;
  const bool canvas = payload.space == MA_PAINT_LAYER_TEXT_SPACE_CANVAS;
  const MaterialPaintLayer *bend_row = BKE_paint_layers_text_decal_bend_row_get(ma, row);
  const bool has_bend_row = payload.bend_enabled != 0 && bend_row != nullptr &&
                            bend_row->curve_patch != nullptr &&
                            bend_row->curve_patch->vectors != nullptr;
  if (canvas) {
    /* Canvas space reads the curve's own UV directly: it is authored in the same canvas the
     * material composite shows, so the text and the curve scale alike and the text may run across
     * UDIM tiles. A canvas text without a usable 2D curve falls back to a straight baseline across
     * the tile. */
    if (has_bend_row && bend_row->curve_patch->stored_space == MA_CURVE_PATCH_SPACE_2D) {
      const PaintVectorItem *item = BKE_paint_vector_curve_patch_item_get(
          *bend_row->curve_patch->vectors);
      const bke::CurvesGeometry *geometry =
          (item != nullptr) ? BKE_paint_vector_item_curve_geometry_get(*item) : nullptr;
      if (geometry != nullptr && geometry->points_num() >= 2 && geometry->curves_num() > 0) {
        geometry->ensure_can_interpolate_to_evaluated();
        const IndexRange curve_points = geometry->evaluated_points_by_curve()[0];
        const Span<float3> evaluated = geometry->evaluated_positions().slice(curve_points);
        if (evaluated.size() >= 2) {
          r_bend.cyclic = geometry->cyclic()[0];
          r_bend.points.resize(evaluated.size());
          for (const int64_t i : evaluated.index_range()) {
            r_bend.points[i] = float2(evaluated[i].x, evaluated[i].y);
          }
          return true;
        }
      }
    }
    r_bend.points.append(float2(0.05f, 0.5f));
    r_bend.points.append(float2(0.95f, 0.5f));
    return true;
  }
  if (payload.bend_enabled == 0) {
    return false;
  }
  if (!has_bend_row) {
    return false;
  }
  const PaintVectorItem *item = BKE_paint_vector_curve_patch_item_get(
      *bend_row->curve_patch->vectors);
  if (item == nullptr) {
    return false;
  }
  const bke::CurvesGeometry *geometry = BKE_paint_vector_item_curve_geometry_get(*item);
  if (geometry == nullptr || geometry->points_num() < 2 || geometry->curves_num() == 0) {
    return false;
  }
  /* A Curve Patch container is a single spline; should one hold more, only the first is the bend
   * curve. The evaluated positions are the shape the layout follows. */
  geometry->ensure_can_interpolate_to_evaluated();
  const IndexRange curve_points = geometry->evaluated_points_by_curve()[0];
  const Span<float3> evaluated = geometry->evaluated_positions().slice(curve_points);
  r_bend.cyclic = geometry->cyclic()[0];
  r_bend.points.resize(evaluated.size());
  if (evaluated.size() < 2) {
    return false;
  }

  if (bend_row->curve_patch->stored_space == MA_CURVE_PATCH_SPACE_2D) {
    /* A projected decal whose bound 2D curve predates the material-UV meaning stores the raster UV
     * itself. Current files never reach this: a 2D bend puts the text in Canvas above. */
    for (const int64_t i : evaluated.index_range()) {
      r_bend.points[i] = float2(evaluated[i].x, evaluated[i].y);
    }
    return true;
  }

  /* A 3D curve is written in the painted object's space and is projected through the decal's
   * frame, then through the same UV adjustments the composite applies to the coverage map: the
   * image-aspect fit and the row's 2D mapping. The text lands where the curve is drawn only when
   * every step the map read takes is mirrored here. */
  TextDecalBendProjector projector;
  if (!text_decal_bend_projector_build(ma, row, projector)) {
    return false;
  }
  for (const int64_t i : evaluated.index_range()) {
    const float p[3] = {evaluated[i].x, evaluated[i].y, evaluated[i].z};
    float uv[2];
    text_decal_bend_uv_from_point(projector, painted_object, p, uv);
    r_bend.points[i] = float2(uv[0], uv[1]);
  }
  return true;
}

bool BKE_paint_layers_text_decal_bend_uv_from_point(const Material &ma,
                                                    const MaterialPaintLayer &text_row,
                                                    const Object *painted_object,
                                                    const float p[3],
                                                    float r_uv[2])
{
  if (text_row.source != MA_PAINT_LAYER_SOURCE_TEXT || text_row.text_decal == nullptr) {
    return false;
  }
  TextDecalBendProjector projector;
  if (!text_decal_bend_projector_build(ma, text_row, projector)) {
    return false;
  }
  text_decal_bend_uv_from_point(projector, painted_object, p, r_uv);
  return true;
}

bool BKE_paint_layers_text_decal_bend_point_from_uv(const Material &ma,
                                                    const MaterialPaintLayer &text_row,
                                                    const Object *painted_object,
                                                    const float uv[2],
                                                    float r_p[3])
{
  if (text_row.source != MA_PAINT_LAYER_SOURCE_TEXT || text_row.text_decal == nullptr) {
    return false;
  }
  TextDecalBendProjector projector;
  if (!text_decal_bend_projector_build(ma, text_row, projector)) {
    return false;
  }
  text_decal_bend_point_from_uv(projector, painted_object, uv, r_p);
  return true;
}

void BKE_paint_layers_text_decal_bend_space_sync(Material &ma,
                                                 MaterialPaintLayer &text_row,
                                                 const bool canvas)
{
  if (text_row.text_decal == nullptr) {
    return;
  }
  if (canvas) {
    text_row.text_decal->space = MA_PAINT_LAYER_TEXT_SPACE_CANVAS;
    /* The canvas text is written and read at raw UV: a mapping left from the projected decal
     * (offset, scale, rotation) would shift the coverage under the curve in the Image Editor. */
    BKE_paint_layers_mapping_reset_raw_uv(ma, &text_row);
  }
  else {
    /* A projected text is read through its Planar frame. A decal that never had one (a hand-written
     * file) gets the same Decal defaults the normal creation path sets. */
    text_row.text_decal->space = MA_PAINT_LAYER_TEXT_SPACE_PROJECTED;
    const bool had_frame = text_row.projection != nullptr;
    BKE_paint_layers_projection_ensure(text_row);
    if (!had_frame) {
      BKE_paint_layers_projection_apply_preset(ma, &text_row, PAINT_LAYER_PROJ_PRESET_DECAL);
    }
    BKE_paint_layers_projection_set_space(
        ma, &text_row, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR);
    text_row.mapping.wrap = MA_PAINT_LAYER_MAPPING_WRAP_CLIP;
  }
  /* The raster follows the curve: re-bake it now so the new Space shows at once. */
  BKE_paint_layers_text_decal_invalidate(ma);
  ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  BKE_paint_layers_tag_edited(ma);
}

/** Everything the bent raster reads from the resolved polyline: its shape and its closure. */
static uint64_t text_decal_bend_hash(const TextDecalBend &bend)
{
  uint64_t h = bake_hash_mix(uint64_t(0x811c9dc59e3779b9ull), bend.cyclic ? 1ull : 0ull);
  h = bake_hash_mix(h, uint32_t(bend.points.size()));
  for (const float2 &point : bend.points) {
    h = bake_hash_float(h, point.x);
    h = bake_hash_float(h, point.y);
  }
  return h;
}

/**
 * Everything the text raster depends on: the source object's identity, its content and formatting,
 * and the payload's bake options. The object's transform is deliberately left out -- the raster
 * lives in the text's own space, and the projection frame is what places it.
 */
static uint64_t text_decal_hash(const MaterialPaintLayerText &payload,
                                const Object &object,
                                const TextDecalBend &bend)
{
  uint64_t h = bake_hash_mix(uint64_t(0x9e3779b97f4a7c15ull), TEXT_DECAL_HASH_VERSION);
  h = bake_hash_mix(h, object.id.session_uid);
  h = bake_hash_mix(h, uint32_t(payload.size));
  h = bake_hash_float(h, payload.padding);
  h = bake_hash_mix(h, uint8_t(payload.fit));
  h = bake_hash_mix(h, uint8_t(payload.space));
  h = bake_hash_float(h, text_decal_bend_font_size_uv(payload));
  /* Old files have no field (zero); they mean the same as an explicit 1.0, so hash the effective
   * value and the two never fight over a re-bake. */
  h = bake_hash_float(h, (payload.font_size > 0.0f) ? payload.font_size : 1.0f);
  /* The bend: the toggle, the options, and the resolved polyline the layout follows. A disabled or
   * unusable bend hashes its zero options, which the straight raster reads the same way. */
  h = bake_hash_mix(h, uint8_t(payload.bend_enabled != 0));
  h = bake_hash_mix(h, uint8_t(payload.bend_mode));
  h = bake_hash_float(h, payload.bend_offset);
  h = bake_hash_float(h, payload.bend_baseline);
  if (!bend.is_empty()) {
    h = bake_hash_mix(h, text_decal_bend_hash(bend));
  }
  const Curve *cu = id_cast<const Curve *>(object.data);
  if (cu == nullptr) {
    return h;
  }
  h = bake_hash_float(h, cu->fsize);
  h = bake_hash_float(h, cu->spacing);
  h = bake_hash_float(h, cu->wordspace);
  h = bake_hash_float(h, cu->linedist);
  h = bake_hash_float(h, cu->shear);
  h = bake_hash_float(h, cu->xof);
  h = bake_hash_float(h, cu->yof);
  h = bake_hash_float(h, cu->ulpos);
  h = bake_hash_float(h, cu->ulheight);
  h = bake_hash_float(h, cu->linewidth);
  h = bake_hash_float(h, cu->smallcaps_scale);
  h = bake_hash_mix(h, uint8_t(cu->spacemode));
  h = bake_hash_mix(h, uint8_t(cu->align_y));
  h = bake_hash_mix(h, uint32_t(cu->totbox));
  for (const int i : IndexRange(cu->totbox)) {
    const TextBox &box = cu->tb[i];
    h = bake_hash_float(h, box.x);
    h = bake_hash_float(h, box.y);
    h = bake_hash_float(h, box.w);
    h = bake_hash_float(h, box.h);
  }
  if (const EditFont *ef = cu->editfont) {
    /* In Edit Mode the live text is the edit buffer; `Curve::str` only catches up on exit. */
    h = bake_hash_mix(h, uint32_t(ef->len));
    for (const int i : IndexRange(ef->len)) {
      h = bake_hash_mix(h, uint32_t(ef->textbuf[i]));
      const CharInfo &info = ef->textbufinfo[i];
      h = bake_hash_mix(h, uint8_t(info.flag));
      h = bake_hash_float(h, info.kern);
    }
  }
  else {
    for (const int i : IndexRange(cu->len)) {
      h = bake_hash_mix(h, uint8_t(cu->str[i]));
    }
    for (const int i : IndexRange(cu->len_char32)) {
      const CharInfo &info = cu->strinfo[i];
      h = bake_hash_mix(h, uint8_t(info.flag));
      h = bake_hash_float(h, info.kern);
    }
  }
  for (const VFont *font : {cu->vfont, cu->vfontb, cu->vfonti, cu->vfontbi}) {
    h = bake_hash_mix(h, font != nullptr ? uint64_t(font->id.session_uid) : uint64_t(0));
    if (font == nullptr) {
      continue;
    }
    h = bake_hash_mix(
        h,
        BLI_hash_mm2(reinterpret_cast<const uchar *>(font->filepath), strlen(font->filepath), 0));
    if (font->packedfile != nullptr) {
      /* Hash the payload's identity rather than its bytes: a packed font is megabytes, and this
       * runs for every Text Decal row on every scene update, animation frames included. The
       * session UID changes when the font is re-read and the pointer when the packed data is
       * replaced, which is when the raster has to be rebuilt. */
      h = bake_hash_mix(h, uint64_t(font->packedfile->size));
      h = bake_hash_mix(h, uint64_t(reinterpret_cast<uintptr_t>(font->packedfile->data)));
    }
  }
  return h;
}

/* -------------------------------------------------------------------- */
/** \name Runtime state
 *
 * A Text Decal's raster is not a row bake, so its state lives in its own map (see
 * `paint_layers_text_decal_intern.hh`) rather than inside #BakeMaterialRuntime. The lock is held
 * around the entry only, never across a raster: the map can rehash while another thread reads it,
 * so no pointer into it outlives the lock.
 * \{ */

Map<uint32_t, TextDecalRuntime> &text_decal_runtime()
{
  static Map<uint32_t, TextDecalRuntime> map;
  return map;
}

std::mutex &text_decal_runtime_mutex()
{
  static std::mutex mutex;
  return mutex;
}

void text_decal_runtime_free(Material &ma)
{
  std::lock_guard lock(text_decal_runtime_mutex());
  const TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(ma.id.session_uid);
  /* Only the owning material may drop the entry: a same-uid old ID freed during a memfile undo
   * must leave the re-read ID's state in place. */
  if (runtime != nullptr && runtime->owner == &ma) {
    text_decal_runtime().remove(ma.id.session_uid);
  }
}

void text_decal_runtime_owner_transfer(Material &dst, Material &src)
{
  std::lock_guard lock(text_decal_runtime_mutex());
  TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(src.id.session_uid);
  if (runtime != nullptr && runtime->owner == &src) {
    runtime->owner = &dst;
  }
}

/** The state of \a marker, or a fresh one. False when another material owns the runtime entry. */
static bool text_decal_state_get(const Material &ma, const bUUID &marker, TextDecalEntry &r_entry)
{
  std::lock_guard lock(text_decal_runtime_mutex());
  TextDecalRuntime &runtime = text_decal_runtime().lookup_or_add(ma.id.session_uid,
                                                                 TextDecalRuntime{});
  if (runtime.owner == nullptr) {
    runtime.owner = &ma;
  }
  if (runtime.owner != &ma) {
    return false;
  }
  for (const TextDecalEntry &entry : runtime.entries) {
    if (BLI_uuid_equal(entry.row, marker)) {
      r_entry = entry;
      return true;
    }
  }
  r_entry = TextDecalEntry{};
  r_entry.row = marker;
  return true;
}

static void text_decal_state_set(const Material &ma, const TextDecalEntry &state)
{
  std::lock_guard lock(text_decal_runtime_mutex());
  TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime == nullptr || runtime->owner != &ma) {
    return;
  }
  for (TextDecalEntry &entry : runtime->entries) {
    if (BLI_uuid_equal(entry.row, state.row)) {
      entry = state;
      return;
    }
  }
  runtime->entries.append(state);
}

/** Drop the states of rows not in \a live. True when one was dropped. */
static bool text_decal_state_prune(const Material &ma, const Span<bUUID> live)
{
  std::lock_guard lock(text_decal_runtime_mutex());
  TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime == nullptr || runtime->owner != &ma) {
    return false;
  }
  bool removed = false;
  for (int i = int(runtime->entries.size()) - 1; i >= 0; i--) {
    const bUUID &row = runtime->entries[i].row;
    bool found = false;
    for (const bUUID &marker : live) {
      if (BLI_uuid_equal(marker, row)) {
        found = true;
        break;
      }
    }
    if (!found) {
      runtime->entries.remove_and_reorder(i);
      removed = true;
    }
  }
  return removed;
}

/** \} */

/**
 * Write the raster's coverage into \a image. An effect's map stores the text as alpha over white,
 * so it reads as the mask of whatever the channels lay; a mask item's map stores the coverage as
 * an opaque grey, the convention of every coverage map. A null raster is an empty text: every
 * texel is transparent, which matters because a missing coverage map reads as full coverage.
 */
static void text_decal_coverage_write(Image &image, const bool is_mask, const ImBuf *raster)
{
  bake_write_image_rows(image, false, nullptr, 0, [&](const int y, const int width, float *r_row) {
    const float *row_pixels = raster != nullptr ?
                                  raster->float_data() + int64_t(y) * raster->x * 4 :
                                  nullptr;
    for (const int x : IndexRange(width)) {
      float alpha = 0.0f;
      if (row_pixels != nullptr) {
        alpha = clamp_f(row_pixels[x * 4 + 3], 0.0f, 1.0f);
      }
      float *pixel = r_row + x * 4;
      if (is_mask) {
        pixel[0] = pixel[1] = pixel[2] = alpha;
        pixel[3] = 1.0f;
      }
      else {
        pixel[0] = pixel[1] = pixel[2] = 1.0f;
        pixel[3] = alpha;
      }
    }
  });
}

/**
 * The map the raster is written into, at \a side: the effect's own coverage map, or a mask item's
 * Base Color map. \a existing is the current one (null when there is none); a map of another size
 * is scaled in place, and one that cannot be scaled is replaced. Null when the map could not be
 * made.
 */
static Image *text_decal_image_ensure(Main &bmain,
                                      Image *existing,
                                      const char *name,
                                      const int side,
                                      const bool is_color,
                                      bool &r_created)
{
  if (existing != nullptr) {
    int width = 0, height = 0;
    BKE_image_get_size(existing, nullptr, &width, &height);
    if (width == side && height == side) {
      return existing;
    }
    /* The payload's size changed. Resize in place rather than replace: the generated tree still
     * names this image until the next regeneration, and replacing it would leave the tree pointing
     * at an orphan (or force a free the shader may still read). */
    if (BKE_image_scale(existing, side, side, nullptr)) {
      return existing;
    }
    /* No buffer to scale (a map whose pixels were never loaded): make a new one below. */
  }
  const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *created = paint_layers_map_create(
      bmain, name, side, is_color, transparent, /*tiled=*/false, /*is_float=*/false);
  r_created = created != nullptr;
  return created;
}

/** The effect's coverage map at \a side, made when missing. The payload owns the born user. */
static Image *text_decal_coverage_ensure(Main &bmain,
                                         MaterialPaintLayer &row,
                                         const int side,
                                         bool &r_created)
{
  MaterialPaintLayerText &payload = *row.text_decal;
  if (payload.coverage != nullptr && payload.coverage->source == IMA_SRC_TILED) {
    /* A projected decal reads one square; a tiled map left by canvas space is replaced. */
    id_us_min(&payload.coverage->id);
    payload.coverage = nullptr;
  }
  char name[192];
  SNPRINTF(name, "%s Coverage", row.name[0] != '\0' ? row.name : "Text Decal");
  bool created = false;
  Image *image = text_decal_image_ensure(bmain, payload.coverage, name, side, false, created);
  if (created) {
    if (payload.coverage != nullptr) {
      /* The unscalable old map is left as an orphan, as a removed row's map is. */
      id_us_min(&payload.coverage->id);
    }
    payload.coverage = image;
    r_created = true;
  }
  return image;
}

/** The UDIM tiles a canvas text may touch: the curve's bbox grown by the font size. */
static void text_decal_canvas_tiles(const TextDecalBend &bend,
                                    const float font_size_uv,
                                    Vector<int> &r_tiles)
{
  r_tiles.clear();
  float2 min(0.0f, 0.0f), max(1.0f, 1.0f);
  if (!bend.points.is_empty()) {
    min = max = bend.points[0];
    for (const float2 &point : bend.points) {
      min.x = std::min(min.x, point.x);
      min.y = std::min(min.y, point.y);
      max.x = std::max(max.x, point.x);
      max.y = std::max(max.y, point.y);
    }
  }
  const float margin = std::max(0.02f, font_size_uv * 1.5f);
  min -= float2(margin);
  max += float2(margin);
  bool outside_udim = false;
  BKE_paint_layers_tiles_from_uv_bounds(min, max, outside_udim, r_tiles);
  if (r_tiles.is_empty()) {
    r_tiles.append(PAINT_TILE_NUMBER_BASE);
  }
}

/** The canvas text's tiled coverage map at \a side, made when missing. */
static Image *text_decal_canvas_image_ensure(Main &bmain,
                                             MaterialPaintLayer &row,
                                             const int side,
                                             const Span<int> tiles,
                                             bool &r_created)
{
  MaterialPaintLayerText &payload = *row.text_decal;
  if (payload.coverage != nullptr) {
    int width = 0, height = 0;
    BKE_image_get_size(payload.coverage, nullptr, &width, &height);
    if (payload.coverage->source != IMA_SRC_TILED || width != side || height != side) {
      id_us_min(&payload.coverage->id);
      payload.coverage = nullptr;
    }
  }
  if (payload.coverage == nullptr) {
    char name[192];
    SNPRINTF(name, "%s Coverage", row.name[0] != '\0' ? row.name : "Text Decal");
    const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Image *image = paint_layers_map_create(
        bmain, name, side, false, transparent, /*tiled=*/true, /*is_float=*/false);
    if (image == nullptr) {
      return nullptr;
    }
    payload.coverage = image;
    r_created = true;
  }
  Image *image = payload.coverage;
  for (const int tile : tiles) {
    if (BKE_image_get_tile(image, tile) == nullptr) {
      BKE_image_add_tile(image, tile, nullptr);
    }
  }
  return image;
}

/** Write one tile of a canvas raster: cover texels where the glyphs are, transparent elsewhere. */
static void text_decal_canvas_write_tile(Image &image,
                                         const int tile_number,
                                         const bool is_mask,
                                         const ImBuf *raster)
{
  bake_write_image_rows(
      image, false, nullptr, tile_number, [&](const int y, const int width, float *r_row) {
        const float *row_pixels = (raster != nullptr) ?
                                      raster->float_data() + int64_t(y) * raster->x * 4 :
                                      nullptr;
        for (const int x : IndexRange(width)) {
          float alpha = 0.0f;
          if (row_pixels != nullptr) {
            alpha = clamp_f(row_pixels[x * 4 + 3], 0.0f, 1.0f);
          }
          float *pixel = r_row + x * 4;
          if (is_mask) {
            pixel[0] = pixel[1] = pixel[2] = alpha;
            pixel[3] = 1.0f;
          }
          else {
            pixel[0] = pixel[1] = pixel[2] = 1.0f;
            pixel[3] = alpha;
          }
        }
      });
}

/** Clear every tile the image already holds, so a moved text leaves no old tile behind. */
static void text_decal_canvas_clear(Image &image, const bool is_mask)
{
  for (const ImageTile &tile : image.tiles) {
    text_decal_canvas_write_tile(image, tile.tile_number, is_mask, nullptr);
  }
}

/**
 * The map of a mask item's Base Color record at \a side, made when missing. A mask item's map is a
 * number, not a colour.
 */
static Image *text_decal_mask_map_ensure(
    Main &bmain, Material &ma, MaterialPaintLayer &row, const int side, bool &r_created)
{
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(row, channel);
  char name[192];
  SNPRINTF(name,
           "%s %s",
           row.name[0] != '\0' ? row.name : "Layer",
           BKE_paint_material_channel_info(channel).ui_name);
  bool created = false;
  Image *image = text_decal_image_ensure(
      bmain, record != nullptr ? record->image : nullptr, name, side, false, created);
  if (created) {
    if (!BKE_paint_layers_channel_set_image(ma, &row, channel, image)) {
      BKE_id_free(&bmain, &image->id);
      return nullptr;
    }
    /* The channel took its own user; the one the image was born with goes. */
    id_us_min(&image->id);
    r_created = true;
  }
  return image;
}

/**
 * Whether the mask item \a row has a live Base Color record to write its grey into. A mask item
 * is the only Text Decal that writes a channel: an effect keeps the user's channel values and
 * textures and writes only its own coverage map.
 */
static bool text_decal_mask_channel_live(const MaterialPaintLayer &row)
{
  return paint_layer_channel_live(
      paint_layer_channel_find(row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

/**
 * Where a Text Decal raster lands. The effect owns a coverage map; a mask item writes the grey
 * into its Base Color record. A third target only has to add another implementation, not another
 * branch in the refresh loop.
 */
class TextDecalSink {
 public:
  virtual ~TextDecalSink() = default;
  /** Whether the row has a live place to write into at all. */
  virtual bool has_target(const MaterialPaintLayer &row) const = 0;
  /** The map at \a side, made when missing; \a r_created says a new map was made. */
  virtual Image *ensure(
      Main &bmain, Material &ma, MaterialPaintLayer &row, int side, bool &r_created) const = 0;
  /** Write the raster's coverage into the map; a null raster is an empty text. */
  virtual void write(Image &image, const ImBuf *raster) const = 0;
};

/** A Text Decal effect: the text is the mask of the whole effect, stored as alpha over white. */
class TextDecalEffectSink : public TextDecalSink {
 public:
  bool has_target(const MaterialPaintLayer & /*row*/) const override
  {
    return true;
  }
  Image *ensure(Main &bmain,
                Material & /*ma*/,
                MaterialPaintLayer &row,
                const int side,
                bool &r_created) const override
  {
    return text_decal_coverage_ensure(bmain, row, side, r_created);
  }
  void write(Image &image, const ImBuf *raster) const override
  {
    text_decal_coverage_write(image, false, raster);
  }
};

/** A mask item: the coverage is the opaque grey of the item's Base Color map. */
class TextDecalMaskSink : public TextDecalSink {
 public:
  bool has_target(const MaterialPaintLayer &row) const override
  {
    return text_decal_mask_channel_live(row);
  }
  Image *ensure(Main &bmain,
                Material &ma,
                MaterialPaintLayer &row,
                const int side,
                bool &r_created) const override
  {
    return text_decal_mask_map_ensure(bmain, ma, row, side, r_created);
  }
  void write(Image &image, const ImBuf *raster) const override
  {
    text_decal_coverage_write(image, true, raster);
  }
};

static const TextDecalEffectSink text_decal_effect_sink;
static const TextDecalMaskSink text_decal_mask_sink;

static const TextDecalSink &text_decal_sink_for(const MaterialPaintLayer &row)
{
  if (row.role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    return text_decal_mask_sink;
  }
  return text_decal_effect_sink;
}

/**
 * The object the raster reads \a payload's content from. The evaluated copy carries animation,
 * drivers, shape keys and modifiers; a live text edit is the exception, because the edit buffer
 * lives on the original object's #Curve.
 */
static Object *text_decal_source_object(const MaterialPaintLayerText &payload,
                                        const Depsgraph *depsgraph)
{
  Object *original = payload.text_object;
  if (original == nullptr || depsgraph == nullptr) {
    return original;
  }
  const Curve *curve = id_cast<const Curve *>(original->data);
  if (curve != nullptr && curve->editfont != nullptr) {
    return original;
  }
  Object *evaluated = DEG_get_evaluated<Object>(depsgraph, original);
  return (evaluated != nullptr && evaluated->type == OB_FONT) ? evaluated : original;
}

/**
 * The public vfont parameters of a Text Decal payload. The DNA enum stops here: the raster module
 * takes its own fit vocabulary, so it never depends on the Paint Layers format.
 */
static VFontRenderFit text_decal_render_fit(const int8_t fit)
{
  return fit == MA_PAINT_LAYER_TEXT_FIT_STRETCH ? VFontRenderFit::Stretch :
                                                  VFontRenderFit::Contain;
}

/** The bend mode, through the same DNA-to-render vocabulary stop as the fit above. */
static VFontRenderBendMode text_decal_render_bend_mode(const int8_t mode)
{
  switch (mode) {
    case MA_PAINT_LAYER_TEXT_BEND_CLIP:
      return VFontRenderBendMode::Clip;
    case MA_PAINT_LAYER_TEXT_BEND_CLAMP:
      return VFontRenderBendMode::Clamp;
    default:
      return VFontRenderBendMode::Fit;
  }
}

/** The timer's identity: its address, shared by every refresh of the process. */
static const char text_decal_wake_anchor = 0;

static uintptr_t text_decal_wake_uuid()
{
  return reinterpret_cast<uintptr_t>(&text_decal_wake_anchor);
}

/**
 * Fires once after the typing pause. Blender does not update by itself when typing stops, so the
 * wake tags the Text objects still being edited; the scene update that follows runs the refresh,
 * which finds the hash stable for long enough and rasterizes. A material whose decal is not held
 * back by a live edit has nothing due and is left alone.
 */
static double text_decal_wake_tick(uintptr_t /*uuid*/, void * /*user_data*/)
{
  if (G_MAIN == nullptr) {
    return -1.0;
  }
  Vector<Object *> due;
  {
    std::lock_guard lock(text_decal_runtime_mutex());
    for (TextDecalRuntime &runtime : text_decal_runtime().values()) {
      if (runtime.owner == nullptr || runtime.present != 1) {
        continue;
      }
      for (Object *object : runtime.editing_sources) {
        const Curve *curve = object != nullptr ? id_cast<const Curve *>(object->data) : nullptr;
        if (curve != nullptr && curve->editfont != nullptr) {
          due.append(object);
        }
      }
    }
  }
  for (Object *object : due) {
    DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  }
  return -1.0;
}

/** (Re)arm the wake-up \a wait seconds from now; a later keystroke re-arms it again. */
static void text_decal_wake_arm(const double wait)
{
  if (BLI_timer_is_registered(text_decal_wake_uuid())) {
    BLI_timer_unregister(text_decal_wake_uuid());
  }
  BLI_timer_register(text_decal_wake_uuid(), text_decal_wake_tick, nullptr, nullptr, wait, false);
}

/** Whether the Text object \a payload reads is being edited (its edit buffer holds the live text).
 */
static bool text_decal_source_editing(const MaterialPaintLayerText &payload)
{
  const Object *original = payload.text_object;
  const Curve *curve = (original != nullptr) ? id_cast<const Curve *>(original->data) : nullptr;
  return curve != nullptr && curve->editfont != nullptr;
}

/**
 * The legacy fallback for a bend whose row was written before the pinned object existed: the first
 * mesh of \a bmain that uses \a ma. A refresh with no editor context (a scene update, a mode exit)
 * resolves it, so the raster of an old 3D bend does not wait for the next operator. New binds and
 * edits pin the object instead (#MaterialPaintLayerText::bend_painted_object), which is
 * deterministic when several objects share the material.
 */
static const Object *text_decal_painted_object_find(Main &bmain, const Material &ma)
{
  for (Object &ob : bmain.objects) {
    if (ob.type != OB_MESH) {
      continue;
    }
    for (int slot = 1; slot <= ob.totcol; slot++) {
      if (BKE_object_material_get(&ob, short(slot)) == &ma) {
        return &ob;
      }
    }
  }
  return nullptr;
}

/**
 * The object whose space a row's 3D bend curve is written in: the object pinned at bind/edit time
 * when it still uses \a ma, else the legacy deterministic scan. The pin keeps a scene with several
 * slots from silently projecting through an arbitrary "first mesh".
 */
static const Object *text_decal_bend_painted_object_get(Main &bmain,
                                                        const Material &ma,
                                                        const MaterialPaintLayer &row)
{
  const Object *pinned = (row.text_decal != nullptr) ? row.text_decal->bend_painted_object :
                                                       nullptr;
  if (pinned != nullptr && pinned->type == OB_MESH &&
      BKE_object_material_slot_find_index(const_cast<Object *>(pinned),
                                          const_cast<Material *>(&ma)) != 0)
  {
    return pinned;
  }

  /* The legacy scan runs for every projected-bend row of a file written before the pin existed.
   * Cache a positive answer per material so a scene update does not pay for it again; resolve it
   * through the session UID, not a stored pointer, so a deleted object is simply a cache miss. An
   * empty answer is not cached: the material may gain a mesh with no tag this refresh can see. */
  uint32_t cached_uid = 0;
  {
    std::lock_guard lock(text_decal_runtime_mutex());
    const TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(ma.id.session_uid);
    if (runtime != nullptr && runtime->owner == &ma) {
      cached_uid = runtime->painted_object_uid;
    }
  }
  if (cached_uid != 0) {
    Object *cached = id_cast<Object *>(
        BKE_libblock_find_session_uid(&bmain, ID_OB, cached_uid));
    if (cached != nullptr && cached->type == OB_MESH &&
        BKE_object_material_slot_find_index(cached, const_cast<Material *>(&ma)) != 0)
    {
      return cached;
    }
  }

  const Object *found = text_decal_painted_object_find(bmain, ma);
  {
    std::lock_guard lock(text_decal_runtime_mutex());
    TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(ma.id.session_uid);
    if (runtime != nullptr && runtime->owner == &ma) {
      /* Keep a positive answer; clear a stale one, but do not cache "none" (see above). */
      runtime->painted_object_uid = (found != nullptr) ? found->id.session_uid : 0;
    }
  }
  return found;
}

void BKE_paint_layers_text_decal_refresh(Main &bmain,
                                         Material &ma,
                                         const Depsgraph *depsgraph,
                                         const Object *painted_object)
{
  if (!BKE_paint_layers_is_layered(ma) || !ID_IS_EDITABLE(&ma.id) ||
      ID_IS_OVERRIDE_LIBRARY(&ma.id))
  {
    return;
  }

  /* A material that answered "no Text Decal" once costs a cached read on every update; only an
   * add, a copy or an undo clears the answer (see #BKE_paint_layers_text_decal_invalidate). */
  {
    std::lock_guard lock(text_decal_runtime_mutex());
    TextDecalRuntime &runtime = text_decal_runtime().lookup_or_add(ma.id.session_uid,
                                                                   TextDecalRuntime{});
    if (runtime.owner == nullptr) {
      runtime.owner = &ma;
    }
    if (runtime.owner != &ma) {
      return;
    }
    if (runtime.present == 0) {
      return;
    }
  }

  /* A Text Decal exists as an effect of a row (a folder included) or as an item of its mask. The
   * rows are collected first: the raster below edits channel records, which the walk must not see
   * move. */
  Vector<MaterialPaintLayer *> rows;
  bool has_text_decal = false;
  const auto collect = [&](MaterialPaintLayer &row) {
    if (row.source != MA_PAINT_LAYER_SOURCE_TEXT || row.text_decal == nullptr) {
      return;
    }
    has_text_decal = true;
    if (row.text_decal->text_object != nullptr) {
      rows.append(&row);
    }
  };
  BKE_paint_layers_foreach(ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) {
    for (MaterialPaintLayer &effect : layer.effects) {
      collect(effect);
    }
    for (MaterialPaintLayer &item : layer.mask_stack) {
      collect(item);
    }
    return true;
  });

  {
    std::lock_guard lock(text_decal_runtime_mutex());
    TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(ma.id.session_uid);
    if (runtime == nullptr || runtime->owner != &ma) {
      return;
    }
    runtime->present = has_text_decal ? 1 : 0;
    /* The wake timer tags the objects whose live edit holds a raster back, not every material
     * with a decal: an object outside Edit Mode cannot produce a new hash on its own. */
    runtime->editing_sources.clear();
    for (const MaterialPaintLayer *row : rows) {
      if (text_decal_source_editing(*row->text_decal)) {
        runtime->editing_sources.append(row->text_decal->text_object);
      }
    }
  }
  if (!has_text_decal) {
    Vector<bUUID> none;
    if (text_decal_state_prune(ma, none)) {
      ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
      DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
    }
    return;
  }

  Vector<bUUID> live;
  bool structure_changed = false;
  /* The nearest time a row held back by a live text edit comes due, or negative for none. */
  double next_wait = -1.0;
  for (MaterialPaintLayer *row : rows) {
    live.append(row->marker);
    MaterialPaintLayerText &payload = *row->text_decal;
    /* A Canvas text must be read at raw UV. A file written between the modes (or a payload changed
     * without the RNA setter) can still carry the projected frame/mapping, which would put the
     * letters where the projection puts them while the curve is authored in material UV. */
    if (payload.space == MA_PAINT_LAYER_TEXT_SPACE_CANVAS &&
        row->mapping.space != MA_PAINT_LAYER_MAPPING_SPACE_UV)
    {
      /* The projection payload is kept for the trip back to Projected; the UV mapping space alone
       * decides the read. */
      BKE_paint_layers_mapping_reset_raw_uv(ma, row);
      ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
      BKE_paint_layers_tag_edited(ma);
    }
    const TextDecalSink &sink = text_decal_sink_for(*row);
    /* A mask item without a live Base Color record has nowhere to write. */
    if (!sink.has_target(*row)) {
      continue;
    }
    const Object &text_object = *text_decal_source_object(payload, depsgraph);
    /* The bend is resolved once per row per refresh: its polyline is both what the hash covers and
     * what the layout follows, so a curve edit and a frame move invalidate the same way. Resolve
     * the object the 3D curve is written in only when a projected bend needs it, so a straight or
     * canvas text never pays for the scan. */
    const Object *row_painted = painted_object;
    if (row_painted == nullptr && payload.bend_enabled != 0 &&
        payload.space != MA_PAINT_LAYER_TEXT_SPACE_CANVAS)
    {
      row_painted = text_decal_bend_painted_object_get(bmain, ma, *row);
    }
    TextDecalBend bend;
    text_decal_bend_build(ma, *row, row_painted, bend);
    /* Only what the raster depends on: a channel value, colour, texture or blend of an effect is
     * no input of it, so editing those never reaches this file's work. */
    const uint64_t hash = text_decal_hash(payload, text_object, bend);
    TextDecalEntry state;
    if (!text_decal_state_get(ma, row->marker, state)) {
      /* Another material owns the runtime entry: this row has no state to write. The scan still
       * finishes so rows that are gone are pruned. */
      continue;
    }
    /* An effect whose coverage map is gone (a copy, an undo) has to be rasterized again. */
    if (state.hash == hash && state.has_raster &&
        (row->role == MA_PAINT_LAYER_ROLE_MASK_ITEM || payload.coverage != nullptr))
    {
      continue;
    }

    /* While the text is being typed, wait for the edit buffer to hold still. A fresh state (hash
     * zero: the first bake, a re-bake, an undo) renders at once, and so does leaving Edit Mode,
     * where the buffer is gone and nothing more can change. */
    if (state.hash != 0 && text_decal_source_editing(payload)) {
      const double now = BLI_time_now_seconds();
      if (state.observed_hash != hash) {
        state.observed_hash = hash;
        state.observed_time = now;
        text_decal_state_set(ma, state);
      }
      const double wait = state.observed_time + TEXT_DECAL_EDIT_DELAY_SECONDS - now;
      if (wait > 0.0) {
        next_wait = (next_wait < 0.0) ? wait : std::min(next_wait, wait);
        continue;
      }
    }

    const int side = BKE_paint_layers_text_decal_size_get(payload);
    state.hash = hash;
    state.observed_hash = hash;
    state.has_raster = false;

    /* Canvas space paints the text into the material's UV canvas, one tile at a time, so it can
     * sit anywhere in the UV layout and cross UDIM tile borders. Each tile is a separate square
     * pass of the same bent layout, shifted by the tile's UV origin: the glyphs that straddle a
     * border are clipped at the same place in both neighbours, so the seam is exact. */
    if (payload.space == MA_PAINT_LAYER_TEXT_SPACE_CANVAS &&
        row->role == MA_PAINT_LAYER_ROLE_EFFECT)
    {
      /* The decal's Size multiplies the canvas base (the bend's own UV size, unchanged for files
       * written before the multiplier existed). 1.0 keeps the historical result. */
      const float size_multiplier = (payload.font_size > 0.0f) ? payload.font_size : 1.0f;
      const float canvas_font_uv = text_decal_bend_font_size_uv(payload) * size_multiplier;
      Vector<int> tiles;
      text_decal_canvas_tiles(bend, canvas_font_uv, tiles);
      bool created = false;
      Image *image = text_decal_canvas_image_ensure(bmain, *row, side, tiles, created);
      structure_changed = structure_changed || created;
      if (image != nullptr) {
        text_decal_canvas_clear(*image, false);
        for (const int tile : tiles) {
          const float2 origin = BKE_image_get_tile_uv_origin(tile);
          Vector<float2> shifted(bend.points.size());
          for (const int64_t i : bend.points.index_range()) {
            shifted[i] = bend.points[i] - origin;
          }
          VFontRenderParams render_params;
          render_params.side = side;
          render_params.padding = payload.padding;
          render_params.fit = text_decal_render_fit(payload.fit);
          render_params.bend.points = shifted.as_span();
          render_params.bend.cyclic = bend.cyclic;
          render_params.bend.offset = payload.bend_offset;
          render_params.bend.baseline = payload.bend_baseline;
          render_params.bend.mode = text_decal_render_bend_mode(payload.bend_mode);
          render_params.bend.glyph_px = canvas_font_uv * float(side);
          render_params.bend.glyph_px_explicit = true;
          ImBuf *raster = BKE_vfont_object_render(text_object, render_params);
          text_decal_canvas_write_tile(*image, tile, false, raster);
          if (raster != nullptr) {
            IMB_freeImBuf(raster);
          }
        }
        state.has_raster = true;
      }
      text_decal_state_set(ma, state);
      continue;
    }

#if PBR_LAYERS_DIAGNOSTICS
    const double start = BLI_time_now_seconds();
#endif
    VFontRenderParams render_params;
    render_params.side = side;
    render_params.padding = payload.padding;
    render_params.fit = text_decal_render_fit(payload.fit);
    /* Size is relative to the automatic fit: 1.0 keeps the decal as it was, 0.5 halves it. Zero
     * reads as 1.0 for a file written before the field existed. */
    render_params.fit_multiplier = (payload.font_size > 0.0f) ? payload.font_size : 1.0f;
    if (!bend.is_empty()) {
      render_params.bend.points = bend.points.as_span();
      render_params.bend.cyclic = bend.cyclic;
    }
    render_params.bend.offset = payload.bend_offset;
    render_params.bend.baseline = payload.bend_baseline;
    render_params.bend.mode = text_decal_render_bend_mode(payload.bend_mode);
    ImBuf *raster = BKE_vfont_object_render(text_object, render_params);
    if (raster == nullptr) {
      /* No text, no font: the decal covers nothing. A missing coverage map would read as full
       * coverage and flood the surface with the channel values. */
      PBR_LAYERS_LOG("[paint-layers text decal] row \"%s\": no raster (empty text or no font)\n",
                     row->name);
    }
    bool created = false;
    Image *image = sink.ensure(bmain, ma, *row, side, created);
    structure_changed = structure_changed || created;
    if (image != nullptr) {
      /* The whole buffer is written, transparent texels included, so no content reset is needed.
       */
      sink.write(*image, raster);
      state.has_raster = true;
    }
    if (raster != nullptr) {
      IMB_freeImBuf(raster);
    }
    text_decal_state_set(ma, state);
    PBR_LAYERS_LOG("[paint-layers text decal] row \"%s\" size=%d time=%.1f ms\n",
                   row->name,
                   side,
                   (BLI_time_now_seconds() - start) * 1000.0);
  }

  /* A row that stopped being a Text Decal drops its state; the map itself is the row's, released
   * by the row's own free path. */
  if (text_decal_state_prune(ma, live)) {
    structure_changed = true;
  }
  if (structure_changed) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
    DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  }
  if (next_wait >= 0.0) {
    text_decal_wake_arm(next_wait);
  }
}

void BKE_paint_layers_text_decal_refresh_tagged(Main &bmain, const Depsgraph &depsgraph)
{
  /* BLF is global state: only the main thread may rasterize. Render workers reach the scene update
   * as well, and the next main-thread update picks the work up. */
  if (!BLI_thread_is_main()) {
    return;
  }
  for (Material &ma : bmain.materials) {
    if (BKE_paint_layers_is_layered(ma)) {
      BKE_paint_layers_text_decal_refresh(bmain, ma, &depsgraph);
    }
  }
}

void BKE_paint_layers_text_decal_invalidate(Material &ma)
{
  std::lock_guard lock(text_decal_runtime_mutex());
  TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime == nullptr || runtime->owner != &ma) {
    return;
  }
  for (TextDecalEntry &entry : runtime->entries) {
    entry.hash = 0;
    entry.has_raster = false;
  }
  /* The stack may have gained or lost a Text Decal under the undo step; the cached answer is not
   * trustworthy any more. */
  runtime->present = -1;
  /* The legacy object scan is re-asked too: an undo can delete or re-add the mesh it picked. */
  runtime->painted_object_uid = 0;
}

}  // namespace blender
