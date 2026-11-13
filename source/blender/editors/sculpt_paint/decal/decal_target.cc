/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The decal target: row groups by name, and the projection frame a viewport view implies, the
 * object-space equivalent of the camera Decal Master projects its decals through.
 */

#include "decal_target.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

#include "BLI_bounds_types.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_report.hh"

#include "DNA_material_types.h"
#include "DNA_object_enums.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../projection/projection_target.hh"

namespace blender::ed::sculpt_paint::decal {

/* -------------------------------------------------------------------- */
/** \name Decal rows
 * \{ */

bool decal_name_of_row_name(const char *row_name, char r_decal[64])
{
  /* The last separator: a channel name never holds one, a decal name may. */
  const char *separator = nullptr;
  for (const char *p = strstr(row_name, DECAL_NAME_SEPARATOR); p != nullptr;
       p = strstr(p + 1, DECAL_NAME_SEPARATOR))
  {
    separator = p;
  }
  if (separator == nullptr || separator == row_name) {
    return false;
  }
  const size_t name_len = size_t(separator - row_name);
  if (name_len >= 64) {
    return false;
  }
  memcpy(r_decal, row_name, name_len);
  r_decal[name_len] = '\0';
  return true;
}

bool decal_name_of_row(const MaterialPaintLayer &row, char r_decal[64])
{
  return decal_name_of_row_name(row.name, r_decal);
}

void decal_row_name_make(const char *decal, const char *channel_name, char r_name[64])
{
  if (decal[0] == '\0') {
    decal = "Decal";
  }
  const size_t tail_len = strlen(DECAL_NAME_SEPARATOR) + strlen(channel_name);
  /* The room left in the 64 byte name (null byte included) for the decal; the UTF-8 copy cuts at a
   * character boundary, never in the middle of one. */
  const size_t head_room = (tail_len < 63) ? 64 - tail_len : 1;
  char head[64];
  BLI_strncpy_utf8(head, decal, head_room);
  BLI_snprintf_utf8(r_name, 64, "%s%s%s", head, DECAL_NAME_SEPARATOR, channel_name);
}

bool decal_mode_supported(const Object &ob)
{
  return (ob.mode & (OB_MODE_SCULPT | OB_MODE_TEXTURE_PAINT)) != 0;
}

void decal_rows_find(Material &ma, const char *decal_name, Vector<MaterialPaintLayer *> &r_rows)
{
  r_rows.clear();
  char name[64];
  for (MaterialPaintLayer &row : ma.paint_layers) {
    if (decal_name_of_row(row, name) && STREQ(name, decal_name)) {
      r_rows.append(&row);
    }
  }
}

Vector<MaterialPaintLayer *> decal_rows_create(Material &ma,
                                               const char *decal_name,
                                               const Span<DecalRowChannel> channels,
                                               MaterialPaintLayer *anchor,
                                               const projection::ProjectionFrame &frame,
                                               const bool clip)
{
  Vector<MaterialPaintLayer *> created;
  for (const DecalRowChannel &entry : channels) {
    if (entry.image == nullptr) {
      continue;
    }
    char row_name[64];
    decal_row_name_make(decal_name, BKE_paint_material_channel_info(entry.channel).ui_name,
                        row_name);
    /* Added bare, not through the Add policy: the policy gives a Fill the default channel set,
     * and the decal row takes part in the one channel the image maps and no other. */
    MaterialPaintLayer *layer = BKE_paint_layers_add(ma, row_name, anchor, PaintLayerPlace::Above);
    if (layer == nullptr) {
      continue;
    }
    layer->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
    if (BKE_paint_layers_channel_add(ma, layer, entry.channel) == nullptr ||
        !BKE_paint_layers_channel_set_image(ma, layer, entry.channel, entry.image))
    {
      BKE_paint_layers_remove(ma, layer);
      continue;
    }
    BKE_paint_layers_projection_set_space(ma, layer, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR);
    created.append(layer);
    /* The next row stacks above the one before it, so the group keeps its list order. */
    anchor = layer;
  }
  if (created.is_empty()) {
    return created;
  }
  /* The rows project through the one frame; a refused frame write leaves the defaults, which
   * still read a valid (unit) frame. */
  decal_frame_apply(ma, created, frame, clip);
  return created;
}

bool decal_target_find(const bContext &C,
                       Material **r_material,
                       Object **r_object,
                       char r_name[64],
                       Vector<MaterialPaintLayer *> *r_rows)
{
  *r_material = nullptr;
  *r_object = nullptr;
  r_name[0] = '\0';
  if (r_rows != nullptr) {
    r_rows->clear();
  }

  Object *ob = CTX_data_active_object(&C);
  if (ob == nullptr || !decal_mode_supported(*ob)) {
    return false;
  }
  Material *ma = BKE_paint_layers_active_material_get(ob);
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma) || !ID_IS_EDITABLE(&ma->id)) {
    return false;
  }

  /* The decal of the shown row wins; with no active decal row, the material must hold exactly one
   * or the operator cannot know which to act on. */
  char shown[64];
  if (MaterialPaintLayer *layer = BKE_paint_layers_active_layer_get(*ma);
      layer != nullptr && decal_name_of_row(*layer, shown))
  {
    *r_material = ma;
    *r_object = ob;
    BLI_strncpy(r_name, shown, 64);
    if (r_rows != nullptr) {
      decal_rows_find(*ma, shown, *r_rows);
    }
    return true;
  }

  char first[64];
  bool found = false;
  char name[64];
  for (MaterialPaintLayer &row : ma->paint_layers) {
    if (!decal_name_of_row(row, name)) {
      continue;
    }
    if (!found) {
      found = true;
      STRNCPY(first, name);
      continue;
    }
    if (!STREQ(first, name)) {
      return false;
    }
  }
  if (!found) {
    return false;
  }
  *r_material = ma;
  *r_object = ob;
  BLI_strncpy(r_name, first, 64);
  if (r_rows != nullptr) {
    decal_rows_find(*ma, first, *r_rows);
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame from the view
 * \{ */

void decal_frame_from_bounds(Object &ob, projection::ProjectionFrame &r_frame)
{
  r_frame = projection::ProjectionFrame();
  const std::optional<Bounds<float3>> bounds = BKE_object_boundbox_get(&ob);
  if (!bounds.has_value()) {
    return;
  }
  mid_v3_v3v3(r_frame.center, bounds->min, bounds->max);
  for (int i = 0; i < 3; i++) {
    /* The frame stores full extents (the projection divides the offset by half of them), so the
     * object's extent is what covers it; the pad keeps a grazing frame off the silhouette. */
    r_frame.size[i] = std::max(float(bounds->max[i] - bounds->min[i]), 0.1f) * 1.05f;
  }
}

bool decal_frame_from_view(const bContext &C,
                           Object &ob,
                           const float mval[2],
                           projection::ProjectionFrame &r_frame)
{
  const ARegion *region = CTX_wm_region(&C);
  const View3D *v3d = CTX_wm_view3d(&C);
  if (region == nullptr || v3d == nullptr || region->regiontype != RGN_TYPE_WINDOW ||
      region->regiondata == nullptr)
  {
    decal_frame_from_bounds(ob, r_frame);
    return false;
  }
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);

  /* The frame faces the view: its basis carries the object's own turn first (the frame values
   * live in object space), then the view's. The transpose is what makes the frame's +Z look back
   * at the viewer, the side a Planar row reads through. */
  float object_from_world[4][4];
  if (!invert_m4_m4(object_from_world, ob.object_to_world().ptr())) {
    decal_frame_from_bounds(ob, r_frame);
    return false;
  }
  float view_from_world[3][3];
  quat_to_mat3(view_from_world, rv3d->viewquat);
  float view_to_world[3][3];
  transpose_m3_m3(view_to_world, view_from_world);
  float object_from_world_3[3][3];
  copy_m3_m4(object_from_world_3, object_from_world);
  float frame_to_object[3][3];
  mul_m3_m3m3(frame_to_object, object_from_world_3, view_to_world);
  /* The basis splits into the normal+roll pair the payload stores. */
  /* The BKE helpers keep a frame's axes in the columns of a [row][column] matrix, the transpose of
   * the native basis (axis per m[i]) the products above build. */
  float frame_basis[3][3];
  transpose_m3_m3(frame_basis, frame_to_object);
  BKE_paint_layers_projection_frame_from_matrix(frame_basis, r_frame.normal, &r_frame.roll);
  float frame_from_object[3][3];
  transpose_m3_m3(frame_from_object, frame_to_object);

  /* Centre at the surface under the cursor; without a hit the object's middle, so the decal still
   * lands where the camera looks. The raycast is the shared one the place and move gestures use. */
  float center[3] = {0.0f, 0.0f, 0.0f};
  bool placed = false;
  if (mval[0] >= 0.0f) {
    float world_loc[3];
    float world_no[3];
    if (projection::projection_frame_raycast(C, region, v3d, ob, mval, world_loc, world_no)) {
      float frame_no[3];
      projection::projection_frame_point_from_world(C, &ob, world_loc, world_no, center, frame_no);
      placed = true;
    }
  }
  if (!placed) {
    if (const std::optional<Bounds<float3>> bounds = BKE_object_boundbox_get(&ob)) {
      mid_v3_v3v3(center, bounds->min, bounds->max);
    }
  }
  copy_v3_v3(r_frame.center, center);

  /* Extents cover the object in frame space, the way the addon normalizes its decal onto the
   * object's bounds; the pad keeps a grazing frame from clipping the silhouette. */
  float half[3] = {0.1f, 0.1f, 0.1f};
  if (const std::optional<Bounds<float3>> bounds = BKE_object_boundbox_get(&ob)) {
    for (const int corner_index : IndexRange(8)) {
      const float3 corner((corner_index & 1) ? bounds->max.x : bounds->min.x,
                          (corner_index & 2) ? bounds->max.y : bounds->min.y,
                          (corner_index & 4) ? bounds->max.z : bounds->min.z);
      float relative[3];
      sub_v3_v3v3(relative, corner, center);
      float corner_frame[3];
      mul_v3_m3v3(corner_frame, frame_from_object, relative);
      for (const int axis : IndexRange(3)) {
        half[axis] = std::max(half[axis], std::abs(corner_frame[axis]));
      }
    }
  }
  for (const int axis : IndexRange(3)) {
    /* Full extents: the distance from the centre to an edge is half of them (the projection
     * divides the offset by that half), so the frame covers the object's silhouette plus the pad. */
    r_frame.size[axis] = half[axis] * 2.0f * 1.05f;
  }
  return true;
}

void decal_frame_apply(Material &ma,
                       const Span<MaterialPaintLayer *> rows,
                       const projection::ProjectionFrame &frame,
                       const bool clip)
{
  for (MaterialPaintLayer *row : rows) {
    BKE_paint_layers_projection_set_axis(ma, row, MA_PAINT_LAYER_PROJECTION_AXIS_Z);
    BKE_paint_layers_projection_set_clip(ma, row, clip);
    BKE_paint_layers_projection_set_values(
        ma, row, frame.center, frame.size, frame.normal, frame.roll);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Group controller and frame conversion
 *
 * A decal group's frame lives in the space of its controller object when one is set, else in the
 * painted object's own space. Anchoring the group hands the frame to a new Empty placed at the
 * frame's world transform, which is what lets one layered material show the decal in one world
 * spot on every one of its users.
 * \{ */

bool decal_group_controller_get(const Span<MaterialPaintLayer *> rows, Object **r_controller)
{
  *r_controller = nullptr;
  bool seen_set = false;
  bool seen_unset = false;
  for (const MaterialPaintLayer *row : rows) {
    if (row->projection_object == nullptr) {
      seen_unset = true;
      continue;
    }
    if (*r_controller != nullptr && *r_controller != row->projection_object) {
      /* Two controllers in one group: no group operator can act on the split. */
      return false;
    }
    *r_controller = row->projection_object;
    seen_set = true;
  }
  /* A mixed group (some rows anchored, some not) is the same disagreement. */
  return !(seen_set && seen_unset);
}

bool decal_frame_convert(const projection::ProjectionFrame &frame,
                         const float4x4 &from_to_world,
                         const float4x4 &world_to_to,
                         projection::ProjectionFrame &r_frame_out,
                         ReportList *reports,
                         const char *from_name,
                         const char *to_name)
{
  const char *const from_label = (from_name != nullptr) ? from_name : "?";
  const char *const to_label = (to_name != nullptr) ? to_name : "?";

  /* The relative transform takes a point of the from-space straight into the to-space. Its
   * invertibility is the degeneracy check: a zero scale would fling the frame away. */
  const float4x4 relative = world_to_to * from_to_world;
  float relative_inverse[4][4];
  if (!invert_m4_m4(relative_inverse, relative.ptr())) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Decal: the frame cannot be converted between '%s' and '%s' -- the relative "
                "transform is degenerate",
                from_label,
                to_label);
    return false;
  }

  projection::ProjectionFrame out;

  /* The relative rotation composes with the frame's own basis; the basis helpers keep it
   * orthonormal, so the result splits back into a normal and a roll. */
  float relative3[3][3];
  copy_m3_m4(relative3, relative.ptr());
  float rot_rel[3][3];
  float scale_rel[3];
  mat3_to_rot_size(rot_rel, scale_rel, relative3);
  float rot_src[3][3];
  BKE_paint_layers_projection_frame_matrix(frame.normal, frame.roll, rot_src);
  /* The relative rotation is a native matrix and the BKE basis is its transpose, so the product is
   * taken in the native layout and turned back for the splitting helper. */
  float basis_src[3][3];
  transpose_m3_m3(basis_src, rot_src);
  float basis_dst[3][3];
  mul_m3_m3m3(basis_dst, rot_rel, basis_src);
  float rot_dst[3][3];
  transpose_m3_m3(rot_dst, basis_dst);
  BKE_paint_layers_projection_frame_from_matrix(rot_dst, out.normal, &out.roll);

  const float3 center_dst = math::transform_point(relative, float3(frame.center));
  copy_v3_v3(out.center, center_dst);

  /* #mat3_to_rot_size negates all three sizes for a mirrored transform (a negative scale, which
   * Blender's decompose folds into the rotation); the extents ride on the magnitudes. */
  const float scale_abs[3] = {
      std::abs(scale_rel[0]), std::abs(scale_rel[1]), std::abs(scale_rel[2])};
  const float scale_max = std::max({scale_abs[0], scale_abs[1], scale_abs[2]});
  const float scale_min = std::min({scale_abs[0], scale_abs[1], scale_abs[2]});
  if (scale_max <= 0.0f) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Decal: the frame cannot be converted between '%s' and '%s' -- the relative "
                "transform has no scale",
                from_label,
                to_label);
    return false;
  }
  if (scale_min > 0.0f && scale_max / scale_min > 1.0f + 1e-4f) {
    /* One warning per conversion, not per row: the caller converts once per space pair. */
    BKE_reportf(reports,
                RPT_WARNING,
                "Decal: the frame is converted between '%s' and '%s' through a non-uniform "
                "transform; the extents follow the largest axis scale",
                from_label,
                to_label);
  }
  for (const int axis : IndexRange(3)) {
    out.size[axis] = frame.size[axis] * scale_max;
  }

  r_frame_out = out;
  return true;
}

/** The name the controller Empty takes: the decal's own, so the Outliner shows what it drives. */
static void decal_controller_name(const Span<MaterialPaintLayer *> rows, char r_name[MAX_NAME])
{
  char decal[64] = "";
  decal_name_of_row(*rows[0], decal);
  /* The destination is a parameter, so a plain SNPRINTF cannot size it: the capacity is explicit. */
  BLI_snprintf(r_name, MAX_NAME, "%s Controller", (decal[0] != '\0') ? decal : "Decal");
}

bool decal_controller_ensure(bContext &C,
                             Material &ma,
                             Object &ob,
                             const Span<MaterialPaintLayer *> rows,
                             ReportList *reports,
                             Object **r_controller)
{
  if (r_controller != nullptr) {
    *r_controller = nullptr;
  }
  if (rows.is_empty()) {
    BKE_report(reports, RPT_ERROR, "No decal rows to anchor");
    return false;
  }
  /* Already anchored: hand the existing controller back instead of creating a second one. A mixed
   * group is a state this helper cannot resolve. */
  Object *existing = nullptr;
  if (!decal_group_controller_get(rows, &existing)) {
    BKE_report(reports, RPT_ERROR, "Decal: its rows disagree about the projection object");
    return false;
  }
  if (existing != nullptr) {
    if (r_controller != nullptr) {
      *r_controller = existing;
    }
    return true;
  }
  /* Validate the whole group before anything is created: the assignment below then cannot refuse
   * halfway, and a refused group leaves no Empty behind. */
  for (const MaterialPaintLayer *row : rows) {
    if (row->projection == nullptr || row->mapping.space == MA_PAINT_LAYER_MAPPING_SPACE_UV) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "Decal: row '%s' does not project, so the group cannot be anchored",
                  row->name);
      return false;
    }
  }
  /* One frame belongs to the group; rows that diverged (the gizmo edits one row) are converted
   * on their own, so anchoring never moves any row's projection. */
  projection::ProjectionFrame first_frame;
  copy_v3_v3(first_frame.center, rows[0]->projection->center);
  copy_v3_v3(first_frame.size, rows[0]->projection->size);
  copy_v3_v3(first_frame.normal, rows[0]->projection->normal);
  first_frame.roll = rows[0]->projection->roll;

  Main *bmain = CTX_data_main(&C);
  Scene *scene = CTX_data_scene(&C);
  ViewLayer *view_layer = CTX_data_view_layer(&C);
  if (bmain == nullptr || scene == nullptr || view_layer == nullptr) {
    BKE_report(reports, RPT_ERROR, "No scene to create the decal controller in");
    return false;
  }

  /* M_empty = M_space · T(c) · R: the Empty takes the frame's own transform, so its space applies
   * the frame math to world points exactly as the row's space did before (the non-uniform scale
   * of either side rides along in the inverse, nothing is lost). */
  const float4x4 space_to_world = projection::projection_space_to_world(C, ob);
  float space_from_world[4][4];
  if (!invert_m4_m4(space_from_world, space_to_world.ptr())) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Decal: object '%s' has a degenerate transform, the group cannot be anchored",
                ob.id.name + 2);
    return false;
  }
  float frame_euler[3];
  BKE_paint_layers_projection_frame_euler(first_frame.normal, first_frame.roll, frame_euler);
  const float4x4 frame_mat = math::from_location<float4x4>(float3(first_frame.center)) *
                             math::from_rotation<float4x4>(math::EulerXYZ(float3(frame_euler)));
  const float4x4 empty_mat = space_to_world * frame_mat;

  /* The active Layer Collection holds the new Empty; without one, the scene's master collection
   * does. #BKE_object_add is not used: it deselects the scene and takes the active object, which
   * a paint operator must not do. */
  Collection *collection = scene->master_collection;
  if (const LayerCollection *lc = BKE_layer_collection_get_active(view_layer);
      lc != nullptr && lc->collection != nullptr)
  {
    collection = lc->collection;
  }

  char empty_name[MAX_NAME];
  decal_controller_name(rows, empty_name);
  Object *empty = BKE_object_add_only_object(bmain, OB_EMPTY, empty_name);
  empty->empty_drawtype = OB_SINGLE_ARROW;
  empty->empty_drawsize = 0.5f;
  if (!BKE_collection_viewlayer_object_add(bmain, view_layer, collection, empty)) {
    BKE_id_free(bmain, empty);
    BKE_reportf(
        reports, RPT_ERROR, "Decal: the controller '%s' could not join the scene", empty_name);
    return false;
  }
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  /* No parent: the Empty is an independent world anchor, its local space is the world's. */
  BKE_object_apply_mat4(empty, empty_mat.ptr(), false, true);
  DEG_id_tag_update(&empty->id, ID_RECALC_TRANSFORM);
  DEG_relations_tag_update(bmain);

  /* Each row's own frame is converted into the Empty's space. The rows normally share one frame,
   * so they all read zero; a row the gizmo moved keeps its own offset instead of being reset. */
  bool empty_inverted = false;
  const float4x4 empty_from_world = math::invert(empty_mat, empty_inverted);
  if (!empty_inverted) {
    BKE_scene_collections_object_remove(bmain, scene, empty, true);
    DEG_relations_tag_update(bmain);
    BKE_report(
        reports, RPT_ERROR, "Decal: the frame's transform is degenerate, the group stays unanchored");
    return false;
  }

  Vector<projection::ProjectionFrame> originals;
  Vector<projection::ProjectionFrame> frames;
  originals.reserve(rows.size());
  frames.reserve(rows.size());
  for (MaterialPaintLayer *row : rows) {
    projection::ProjectionFrame src;
    copy_v3_v3(src.center, row->projection->center);
    copy_v3_v3(src.size, row->projection->size);
    copy_v3_v3(src.normal, row->projection->normal);
    src.roll = row->projection->roll;
    projection::ProjectionFrame dst;
    if (!decal_frame_convert(src,
                             space_to_world,
                             empty_from_world,
                             dst,
                             reports,
                             ob.id.name + 2,
                             empty_name))
    {
      BKE_scene_collections_object_remove(bmain, scene, empty, true);
      DEG_relations_tag_update(bmain);
      return false;
    }
    originals.append(src);
    frames.append(dst);
  }

  /* Controller first, values after: the controller assignment is structural (it rebuilds the
   * material's graph), the values are a value-only edit on top. */
  for (const int i : rows.index_range()) {
    MaterialPaintLayer *row = rows[i];
    if (!BKE_paint_layers_projection_set_object(ma, row, empty) ||
        !BKE_paint_layers_projection_set_values(
            ma, row, frames[i].center, frames[i].size, frames[i].normal, frames[i].roll))
    {
      /* Roll the whole group back: a half-anchored decal would project in two spaces at once.
       * Every row that already points at the new Empty is reset, the failing one included (its
       * controller assignment may have gone through before its values were refused). */
      for (const int j : rows.index_range()) {
        MaterialPaintLayer *other = rows[j];
        if (other->projection_object == empty) {
          BKE_paint_layers_projection_set_object(ma, other, nullptr);
        }
        /* The row still carries its object-space frame or the not-yet-applied converted one. */
        BKE_paint_layers_projection_set_values(
            ma, other, originals[j].center, originals[j].size, originals[j].normal, originals[j].roll);
      }
      /* The rows let the Empty go above; removing it from the scene frees it (its user count was
       * the collection's membership plus the rows'). */
      BKE_scene_collections_object_remove(bmain, scene, empty, true);
      DEG_relations_tag_update(bmain);
      BKE_reportf(reports,
                  RPT_ERROR,
                  "Decal: row '%s' refused the controller, the group stays unanchored",
                  row->name);
      return false;
    }
  }
  /* Only after the group took the Empty: a notifier would otherwise outlive a rolled-back one. */
  WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, empty);

  if (r_controller != nullptr) {
    *r_controller = empty;
  }
  return true;
}

bool decal_controller_detach(const bContext &C,
                             Material &ma,
                             Object &ob,
                             const Span<MaterialPaintLayer *> rows,
                             ReportList *reports,
                             const bool delete_empty)
{
  Object *controller = nullptr;
  if (!decal_group_controller_get(rows, &controller) || controller == nullptr) {
    BKE_report(reports,
               RPT_ERROR,
               "Decal: its rows do not share one controller to release");
    return false;
  }
  for (const MaterialPaintLayer *row : rows) {
    if (row->projection == nullptr || row->mapping.space == MA_PAINT_LAYER_MAPPING_SPACE_UV) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "Decal: row '%s' does not project, so the group cannot be released",
                  row->name);
      return false;
    }
  }

  /* Each row's frame reads in the controller's space while the controller is still set; the
   * converted values are written back before the controller goes. A row the gizmo moved keeps its
   * own offset: the conversion runs per row, so releasing never flattens the group onto one frame. */
  const float4x4 controller_to_world = projection::projection_space_to_world(C, *controller);
  const float4x4 world_to_object = projection::projection_space_from_world(C, ob);
  Vector<projection::ProjectionFrame> originals;
  Vector<projection::ProjectionFrame> converted;
  originals.reserve(rows.size());
  converted.reserve(rows.size());
  for (MaterialPaintLayer *row : rows) {
    projection::ProjectionFrame src;
    copy_v3_v3(src.center, row->projection->center);
    copy_v3_v3(src.size, row->projection->size);
    copy_v3_v3(src.normal, row->projection->normal);
    src.roll = row->projection->roll;
    projection::ProjectionFrame dst;
    if (!decal_frame_convert(src,
                             controller_to_world,
                             world_to_object,
                             dst,
                             reports,
                             controller->id.name + 2,
                             ob.id.name + 2))
    {
      return false;
    }
    originals.append(src);
    converted.append(dst);
  }

  /* Write every row before dropping a controller: a refusal halfway leaves rows whose values are
   * in the object's space while their controller still says otherwise. */
  int written = 0;
  for (const int i : rows.index_range()) {
    MaterialPaintLayer *row = rows[i];
    if (!BKE_paint_layers_projection_set_values(ma,
                                                row,
                                                converted[i].center,
                                                converted[i].size,
                                                converted[i].normal,
                                                converted[i].roll))
    {
      for (const int j : IndexRange(written)) {
        BKE_paint_layers_projection_set_values(ma,
                                               rows[j],
                                               originals[j].center,
                                               originals[j].size,
                                               originals[j].normal,
                                               originals[j].roll);
      }
      BKE_reportf(reports,
                  RPT_ERROR,
                  "Decal: row '%s' refused the converted frame, the group stays anchored",
                  row->name);
      return false;
    }
    written++;
  }
  for (MaterialPaintLayer *row : rows) {
    BKE_paint_layers_projection_set_object(ma, row, nullptr);
  }

  if (delete_empty) {
    Main *bmain = CTX_data_main(&C);
    Scene *scene = CTX_data_scene(&C);
    if (bmain != nullptr && scene != nullptr) {
      bool parented = false;
      for (const Object &candidate : bmain->objects) {
        if (candidate.parent == controller) {
          parented = true;
          break;
        }
      }
      /* One user is the collection membership: nothing else holds the Empty, so it can go. The
       * active object is never removed -- the editor context may still be painting through it. */
      if (controller->id.us <= 1 && !parented && CTX_data_active_object(&C) != controller) {
        BKE_scene_collections_object_remove(bmain, scene, controller, true);
        DEG_relations_tag_update(bmain);
        WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, nullptr);
      }
      else {
        BKE_reportf(reports,
                    RPT_INFO,
                    "Decal: the controller '%s' still has users and is kept",
                    controller->id.name + 2);
      }
    }
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Propagation targets
 *
 * The decal group travels to the layered material of every other selected object: the copies
 * either share the source's controller (one frame edit everywhere) or take the frame converted
 * into each object's own space.
 * \{ */

void decal_propagation_candidates(const bContext &C,
                                  const Object &source,
                                  Vector<Object *> &r_candidates)
{
  r_candidates.clear();
  Vector<PointerRNA> selected;
  CTX_data_selected_editable_objects(&C, &selected);
  Set<Object *> seen;
  for (const PointerRNA &ptr : selected) {
    Object *ob = static_cast<Object *>(ptr.data);
    if (ob == nullptr || ob == &source) {
      continue;
    }
    if (seen.add(ob)) {
      r_candidates.append(ob);
    }
  }
}

bool decal_rows_copy(Main &bmain,
                     const bContext &C,
                     Material & /*src_ma*/,
                     Object &src_ob,
                     Material &dst_ma,
                     Object &dst_ob,
                     const Span<MaterialPaintLayer *> rows,
                     const DecalCopyMode mode,
                     ReportList *reports,
                     Vector<MaterialPaintLayer *> *r_copies)
{
  if (r_copies != nullptr) {
    r_copies->clear();
  }
  if (rows.is_empty()) {
    return false;
  }
  char decal_name[64] = "";
  if (!decal_name_of_row(*rows[0], decal_name)) {
    STRNCPY(decal_name, "Decal");
  }

  bool copied = false;
  MaterialPaintLayer *previous = nullptr;
  for (MaterialPaintLayer *row : rows) {
    if (mode == DecalCopyMode::ConvertFrame && row->projection == nullptr) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "Decal '%s': row '%s' skipped -- it carries no frame to convert",
                  decal_name,
                  row->name);
      continue;
    }
    bool channels_in_set = true;
    for (const int i : IndexRange(row->channels_num)) {
      if (!BKE_paint_layers_channel_in_set(dst_ma,
                                           eMaterialPaintChannel(row->channels[i].channel)))
      {
        channels_in_set = false;
        break;
      }
    }
    if (!channels_in_set) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "Decal '%s': row '%s' skipped -- channel is not in the material's set",
                  decal_name,
                  row->name);
      continue;
    }

    /* The shared image is the point of the copy: the decal's map is one content every material
     * shows, while the masks travel as the row's own shape. */
    MaterialPaintLayer *copy = BKE_paint_layers_copy_to_material(
        bmain, dst_ma, *row, previous, PaintLayerPlace::Above, true);
    if (copy == nullptr) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "Decal '%s': row '%s' could not be copied",
                  decal_name,
                  row->name);
      continue;
    }

    if (mode == DecalCopyMode::ConvertFrame) {
      /* The frame is read in the source's space -- the controller when one is set, else the
       * source object -- and rewritten in the target object's own space; the controller the copy
       * took from the payload is given back. */
      const Object *space_ob = (row->projection_object != nullptr) ? row->projection_object :
                                                                    &src_ob;
      projection::ProjectionFrame src_frame;
      copy_v3_v3(src_frame.center, row->projection->center);
      copy_v3_v3(src_frame.size, row->projection->size);
      copy_v3_v3(src_frame.normal, row->projection->normal);
      src_frame.roll = row->projection->roll;
      projection::ProjectionFrame dst_frame;
      if (!decal_frame_convert(src_frame,
                               projection::projection_space_to_world(C, *space_ob),
                               projection::projection_space_from_world(C, dst_ob),
                               dst_frame,
                               reports,
                               space_ob->id.name + 2,
                               dst_ob.id.name + 2))
      {
        BKE_paint_layers_remove(dst_ma, copy);
        continue;
      }
      if (!BKE_paint_layers_projection_set_values(
              dst_ma, copy, dst_frame.center, dst_frame.size, dst_frame.normal, dst_frame.roll) ||
          !BKE_paint_layers_projection_set_object(dst_ma, copy, nullptr))
      {
        /* The copy kept the source's controller while its values are in the target's space: it
         * would project in the wrong place, so it is dropped rather than left half-converted. */
        BKE_paint_layers_remove(dst_ma, copy);
        BKE_reportf(reports,
                    RPT_WARNING,
                    "Decal '%s': row '%s' could not take the converted frame",
                    decal_name,
                    row->name);
        continue;
      }
    }

    previous = copy;
    copied = true;
    if (r_copies != nullptr) {
      r_copies->append(copy);
    }
  }
  return copied;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::decal
