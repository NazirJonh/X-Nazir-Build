/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <functional>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_function_ref.hh"
#include "BLI_hash.hh"
#include "BLI_implicit_sharing.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_path_utils.hh"
#include "BLI_polyfill_2d.h"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_attribute.hh"
#include "BKE_blender.hh"
#include "BKE_customdata.hh"
#include "BKE_editmesh.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_layer.hh"
#include "BKE_library.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_screen.hh"
#include "BKE_undo_system.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "bmesh.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"
#include "ED_uvedit.hh"

#include "BIF_glutil.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "BLF_api.hh"
#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../../space_image/image_runtime.hh"
#include "../paint_intern.hh"
#include "paint_face_selection_mask.hh"
#include "paint_image_select_gesture.hh"
#include "paint_image_select_gradient.hh"
#include "paint_image_select_intern.hh"
/* #image_select_move_delegate_to_move_operator only. */
#include "paint_image_select_move_intern.hh"
/* #ED_image_paint_select_transform_state_free only. */
#include "paint_image_select_transform_intern.hh"
#include "paint_image_uv_geom.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Internal helpers
 * \{ */

Vector<ImagePaintSelectionTarget> image_paint_selection_targets_get(const bContext *C,
                                                                     const SpaceImage *sima)
{
  Vector<ImagePaintSelectionTarget> targets;
  if (!sima || !sima->image) {
    return targets;
  }

  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);
  if (!scene || !ob ||
      scene->toolsettings->paint_mode.canvas_source != PAINT_CANVAS_SOURCE_MATERIAL)
  {
    targets.append({sima->image, sima->iuser});
    return targets;
  }

  const Brush *brush = BKE_paint_brush(&scene->toolsettings->imapaint.paint);
  const BrushMaterialPaint *brush_paint = brush ? brush->material_paint : nullptr;
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  const Vector<PaintMaterialImageTarget> material_targets = BKE_paint_material_image_targets_get(
      *ob,
      paint_mode,
      brush_paint,
      scene->toolsettings->imapaint.paint.visible_material_channels);

  for (const PaintMaterialImageTarget &material_target : material_targets) {
    if (!material_target.image || !ID_IS_EDITABLE(material_target.image) ||
        ID_IS_OVERRIDE_LIBRARY(material_target.image))
    {
      continue;
    }
    if (std::any_of(targets.begin(), targets.end(), [&](const ImagePaintSelectionTarget &target) {
          return target.image == material_target.image;
        }))
    {
      continue;
    }
    targets.append({material_target.image,
                    material_target.iuser ? *material_target.iuser : sima->iuser});
  }

  /* A temporarily unresolved Material canvas must not make selection tools unusable. It retains
   * the single-image behavior until the configured PBR targets become available again. */
  if (targets.is_empty()) {
    targets.append({sima->image, sima->iuser});
  }
  else {
    for (const int i : targets.index_range()) {
      if (targets[i].image == sima->image) {
        std::swap(targets[0], targets[i]);
        break;
      }
    }
  }
  return targets;
}

void image_paint_selection_undo_begin(const char *name,
                                      const Span<ImagePaintSelectionTarget> targets)
{
  ED_image_undo_push_begin(name, PaintMode::Texture2D);
  for (const ImagePaintSelectionTarget &target : targets) {
    for (const ImageTile *tile : ListBaseWrapper<ImageTile>(target.image->tiles)) {
      ED_image_undo_capture_selection_mask(target.image, tile->tile_number);
    }
  }
}

void image_paint_selection_targets_update(bContext *C,
                                          const Span<ImagePaintSelectionTarget> targets)
{
  for (const ImagePaintSelectionTarget &target : targets) {
    BKE_image_paint_selection_blend_mask_invalidate(target.image);
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, target.image);
  }
  if (Scene *scene = CTX_data_scene(C)) {
    DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
    DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);
  }
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
}

/**
 * Poll for all image paint selection operators.
 * Does not require an active brush -- selection tools are independent of the brush.
 * Blocked while any floating paint-select session is live so stray LMB cannot start a new
 * selection gesture (or a brush stroke) over lifted pixels. Move→transform hand-over does not
 * go through this poll. Also blocked when a *different* Image Editor has this Image's canvas
 * floating, so a second editor cannot gesture into the holes that session left behind (see
 * #image_select_canvas_borrowed_elsewhere).
 */
bool image_paint_selection_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima) {
    return false;
  }
  if (sima->mode != SI_MODE_PAINT) {
    return false;
  }
  if (sima->image != nullptr &&
      (!ID_IS_EDITABLE(sima->image) || ID_IS_OVERRIDE_LIBRARY(sima->image)))
  {
    return false;
  }
  if (image_select_session_active(sima)) {
    return false;
  }
  if (image_select_canvas_borrowed_elsewhere(sima->image, sima)) {
    return false;
  }
  const ARegion *region = CTX_wm_region(C);
  if (!region || region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }
  return true;
}

/** Return true if any mask pixel covered by \a efa exceeds \a threshold. */
static bool face_uv_tri_intersects_mask(const BMFace *efa,
                                        const BMUVOffsets &offsets,
                                        const float2 &uv_origin,
                                        const ImBuf *mask,
                                        const float threshold)
{
  const float *data = mask->float_data();
  const int width = mask->x;

  bool found = false;
  foreach_face_pixel(efa,
                     offsets,
                     uv_origin,
                     width,
                     mask->y,
                     [&](const int x, const int y, const bool /*strict*/) {
                       if (data[y * width + x] > threshold) {
                         found = true;
                         return false;
                       }
                       return true;
                     });
  return found;
}

/** Write \a fill_value to every mask pixel covered by \a efa. */
static void rasterize_face_to_mask(const BMFace *efa,
                                   const BMUVOffsets &offsets,
                                   const float2 &uv_origin,
                                   ImBuf *mask,
                                   const float fill_value)
{
  float *data = mask->float_data_for_write();
  const int width = mask->x;

  foreach_face_pixel(efa,
                     offsets,
                     uv_origin,
                     width,
                     mask->y,
                     [&](const int x, const int y, const bool /*strict*/) {
                       data[y * width + x] = fill_value;
                       return true;
                     });
}

/** Same image datablock, or the same file on disk (duplicate Image IDs are common). */
static bool image_is_paint_target(const Image *target, const Image *candidate)
{
  if (!target || !candidate) {
    return false;
  }
  if (target == candidate) {
    return true;
  }
  if (BKE_image_has_filepath(target) && BKE_image_has_filepath(candidate)) {
    char path_target[FILE_MAX];
    char path_candidate[FILE_MAX];
    BKE_image_user_file_path(nullptr, target, path_target);
    BKE_image_user_file_path(nullptr, candidate, path_candidate);
    if (path_target[0] && path_candidate[0] && BLI_path_cmp(path_target, path_candidate) == 0) {
      return true;
    }
  }
  return false;
}

/** Recursive search for an Image Texture node referencing \a image (includes node groups). */
static bool nodetree_uses_image(const bNodeTree *ntree, const Image *image)
{
  for (const bNode *node : ntree->all_nodes()) {
    if (node->type_legacy == SH_NODE_TEX_IMAGE && node->id) {
      if (image_is_paint_target(image, id_cast<Image *>(node->id))) {
        return true;
      }
    }
    if (ELEM(node->type_legacy, NODE_GROUP, NODE_CUSTOM_GROUP) && node->id) {
      if (nodetree_uses_image(id_cast<const bNodeTree *>(node->id), image)) {
        return true;
      }
    }
  }
  return false;
}

/** True when \a ma references \a image (paint slots or shader nodes). */
static bool material_uses_image(Material *ma, Object *ob, Scene *scene, const Image *image)
{
  if (!ma) {
    return false;
  }

  /* Nodetree first: BKE_texpaint_slot_refresh_cache skips slot build in IMAGE paint mode. */
  if (ma->nodetree && nodetree_uses_image(ma->nodetree, image)) {
    return true;
  }

  if (ma->texpaintslot == nullptr) {
    BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  }

  if (ma->texpaintslot) {
    for (const int s : IndexRange(ma->tot_slots)) {
      if (image_is_paint_target(image, ma->texpaintslot[s].ima)) {
        return true;
      }
    }
  }

  return false;
}

static bool mesh_object_has_uv_maps(const Object *ob)
{
  if (!ob || ob->type != OB_MESH) {
    return false;
  }
  const Mesh *mesh = id_cast<const Mesh *>(ob->data);
  return mesh && !mesh->uv_map_names().is_empty();
}

/** True when \a ob has a UV layout on its Mesh or (in Edit Mode) on the BMesh. */
static bool mesh_object_has_uv_layout(Object *ob)
{
  if (!ob || ob->type != OB_MESH) {
    return false;
  }
  if (ob->mode & OB_MODE_EDIT) {
    BMEditMesh *em = BKE_editmesh_from_object(ob);
    if (em && em->bm) {
      const BMUVOffsets offsets = BM_uv_map_offsets_get(em->bm);
      if (offsets.uv >= 0) {
        return true;
      }
    }
  }
  return mesh_object_has_uv_maps(ob);
}

/**
 * True when \a ob is a mesh that paints onto \a image (material slots, imapaint, or paint_mode).
 */
static bool mesh_object_uses_image(Object *ob, Scene *scene, const Image *image)
{
  if (!ob || !scene || !scene->toolsettings || ob->type != OB_MESH) {
    return false;
  }

  Image *canvas = nullptr;
  ImageUser *canvas_image_user = nullptr;
  if (BKE_paint_canvas_image_get(
          &scene->toolsettings->paint_mode, ob, &canvas, &canvas_image_user) &&
      image_is_paint_target(image, canvas))
  {
    return true;
  }

  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;
  if (image_is_paint_target(image, imapaint.canvas)) {
    return true;
  }

  if (imapaint.mode == IMAGEPAINT_MODE_IMAGE && mesh_object_has_uv_maps(ob)) {
    /* Image Editor paints sima->image; imapaint.canvas may point at another datablock. */
    return true;
  }

  for (const int i : IndexRange(ob->totcol)) {
    if (material_uses_image(BKE_object_material_get(ob, i + 1), ob, scene, image)) {
      return true;
    }
  }

  return false;
}

static void image_paint_selection_object_add_unique(Vector<Object *> &objects, Object *ob)
{
  if (!ob || ob->type != OB_MESH) {
    return;
  }
  for (const Object *existing : objects) {
    if (existing == ob) {
      return;
    }
  }
  objects.append(ob);
}

struct ImagePaintObjectCollectData {
  const Image *image;
  Main *bmain;
  ViewLayer *view_layer;
  Vector<Object *> *objects;
};

static void image_paint_object_collect_from_user(Image *ima,
                                                 ID *id,
                                                 ImageUser * /*iuser*/,
                                                 void *userdata)
{
  ImagePaintObjectCollectData *data = static_cast<ImagePaintObjectCollectData *>(userdata);
  if (!image_is_paint_target(data->image, ima) || id == nullptr) {
    return;
  }

  auto try_add = [&](Object *ob) {
    if (ob && BKE_view_layer_base_find(data->view_layer, ob)) {
      image_paint_selection_object_add_unique(*data->objects, ob);
    }
  };

  switch (GS(id->name)) {
    case ID_OB:
      try_add(id_cast<Object *>(id));
      break;
    case ID_MA: {
      Material *ma = id_cast<Material *>(id);
      for (Object *ob = static_cast<Object *>(data->bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next))
      {
        if (!BKE_view_layer_base_find(data->view_layer, ob)) {
          continue;
        }
        for (const int i : IndexRange(ob->totcol)) {
          if (BKE_object_material_get(ob, i + 1) == ma) {
            try_add(ob);
            break;
          }
        }
      }
      break;
    }
    default:
      break;
  }
}

Vector<Object *> image_paint_selection_canvas_objects_get(const bContext *C,
                                                          const Image *image,
                                                          const ImagePaintCanvasPurpose purpose)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects;

  const auto add_if_uses_image = [&](Object *ob) {
    if (!mesh_object_uses_image(ob, scene, image)) {
      return;
    }
    image_paint_selection_object_add_unique(objects, ob);
  };

  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);

  /* Find meshes whose materials/nodes reference this image (same walk as BKE_image reload). */
  ImagePaintObjectCollectData walk_data{};
  walk_data.image = image;
  walk_data.bmain = bmain;
  walk_data.view_layer = view_layer;
  walk_data.objects = &objects;
  BKE_image_walk_all_users(bmain, &walk_data, image_paint_object_collect_from_user);

  for (Base &base : *BKE_view_layer_object_bases_get(view_layer)) {
    if (base.object) {
      add_if_uses_image(base.object);
    }
  }

  if (purpose == ImagePaintCanvasPurpose::Fill) {
    /* A fill writes pixels. Adding an object that merely overlaps in UV space would
     * stamp an unrelated layout into the texture, so stop at objects that really use
     * this image; the active object still qualifies through the same test. */
    add_if_uses_image(CTX_data_active_object(C));
    return objects;
  }

  /* Image Editor paints `sima->image` directly and does not require the image to be assigned to a
   * material. Filtering these through #mesh_object_uses_image drops every candidate in the typical
   * "unwrap, open image, paint" workflow, and SET+UV-Island then writes an empty mask because it
   * skips gesture rasterization. Safe here only because a mask is reversible. */

  for (Object *ob : BKE_view_layer_array_from_objects_in_edit_mode_unique_data_with_uvs(
           *bmain, scene, view_layer, nullptr))
  {
    image_paint_selection_object_add_unique(objects, ob);
  }

  for (Object *ob : BKE_view_layer_array_from_objects_in_mode_unique_data(
           *bmain, scene, view_layer, nullptr, OB_MODE_TEXTURE_PAINT))
  {
    if (mesh_object_has_uv_layout(ob)) {
      image_paint_selection_object_add_unique(objects, ob);
    }
  }

  Object *active = CTX_data_active_object(C);
  if (!active) {
    active = BKE_view_layer_active_object_get(view_layer);
  }
  if (mesh_object_has_uv_layout(active)) {
    image_paint_selection_object_add_unique(objects, active);
  }

  return objects;
}

/**
 * The UV-map name candidates the paint canvas may be sampled through for \a ob, in priority order:
 * the active material slot's UV override first (image paint Material mode), then
 * #BKE_paint_canvas_uvmap_name_get.
 *
 * The names are deliberately not checked here: the mesh and the BMesh keep separate layer stores (a
 * UV map created or renamed in Edit Mode lives in the BMesh until it is flushed), so an existence
 * check must run against the caller's own data. #image_paint_canvas_mesh_uv_name_get does it for
 * the mesh, `image_paint_selection_uv_offsets_get` for the BMesh.
 */
static Vector<std::string> image_paint_canvas_uv_name_candidates(const Scene &scene, Object &ob)
{
  Vector<std::string> candidates;

  const ImagePaintSettings &imapaint = scene.toolsettings->imapaint;
  if (imapaint.mode == IMAGEPAINT_MODE_MATERIAL) {
    Material *ma = BKE_object_material_get(&ob, ob.actcol);
    if (ma && ma->texpaintslot && ma->paint_active_slot < ma->tot_slots) {
      const char *uvname = ma->texpaintslot[ma->paint_active_slot].uvname;
      if (uvname && uvname[0]) {
        candidates.append(std::string(uvname));
      }
    }
  }
  if (const std::optional<StringRef> uv_ref = BKE_paint_canvas_uvmap_name_get(
          &scene.toolsettings->paint_mode, &ob))
  {
    if (!uv_ref->is_empty()) {
      candidates.append(std::string(*uv_ref));
    }
  }
  return candidates;
}

/**
 * The first UV-map candidate of #image_paint_canvas_uv_name_candidates that exists as a
 * corner-domain float2 attribute on \a mesh, or no value. A material slot can keep pointing at a
 * renamed or deleted UV map, and rasterizing the derived masks through a missing layer would
 * silently produce an empty mask.
 */
static std::optional<std::string> image_paint_canvas_mesh_uv_name_get(const Scene &scene,
                                                                      Object &ob,
                                                                      const Mesh &mesh)
{
  const bke::AttributeAccessor attributes = mesh.attributes();
  for (const std::string &name : image_paint_canvas_uv_name_candidates(scene, ob)) {
    if (attributes.lookup<float2>(name, bke::AttrDomain::Corner)) {
      return name;
    }
  }
  return std::nullopt;
}

BMUVOffsets image_paint_selection_uv_offsets_get(BMesh *bm, Object *ob, const Scene *scene)
{
  for (const std::string &uv_name : image_paint_canvas_uv_name_candidates(*scene, *ob)) {
    const int layer = CustomData_get_named_layer_index(
        &bm->ldata, CD_PROP_FLOAT2, uv_name.c_str());
    if (layer != -1) {
      return BM_uv_map_offsets_from_layer(bm, layer);
    }
  }

  return BM_uv_map_offsets_get(bm);
}

/**
 * Return true if the UV bounding box of \a efa overlaps \a uv_rect.
 * Used for geometric seed detection in subtract mode.
 */
static bool face_uv_intersects_rect(const BMFace *efa,
                                    const BMUVOffsets &offsets,
                                    const rctf &uv_rect)
{
  rctf face_uv_bounds;
  BLI_rctf_init_minmax(&face_uv_bounds);
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, const_cast<BMFace *>(efa), BM_LOOPS_OF_FACE) {
    const float *uv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
    BLI_rctf_do_minmax_v(&face_uv_bounds, uv);
  }
  return BLI_rctf_isect(&face_uv_bounds, &uv_rect, nullptr);
}

/**
 * \param gesture_uv_bounds: When non-null, seed faces are those whose UV bounds intersect the
 * gesture region in UV space (box/lasso/circle). This matches Image Editor selection geometry.
 * \param expand_islands: When true, flood-fill from the seed faces to their UV islands. When
 * false, only the seed faces themselves are rasterized (Face expand mode).
 */
static void image_paint_selection_expand_for_object(Scene *scene,
                                                    Object *ob,
                                                    Image *image,
                                                    const float fill_value,
                                                    const float threshold,
                                                    const rctf *gesture_uv_bounds,
                                                    const bool expand_islands)
{
  BMesh *bm = nullptr;
  bool owns_bm = false;

  if (ob->mode & OB_MODE_EDIT) {
    BMEditMesh *em = BKE_editmesh_from_object(ob);
    if (!em) {
      return;
    }
    bm = em->bm;
  }
  else {
    /* Deliberately the *original* mesh, not the evaluated one. This expansion works purely in UV
     * space on the UV layout the user authored and sees in the UV editor; the evaluated mesh has
     * post-modifier topology (subdivision, mirror, array) whose face count, face indices and UV
     * island connectivity need not match it, so seeding and flood-filling there would grow the
     * selection over geometry that does not exist in the layout being painted. It also keeps this
     * branch consistent with the edit-mode branch above, which already borrows the original
     * `em->bm`, and makes the UV-layer lookup by name below reliable. */
    const Mesh *mesh = id_cast<const Mesh *>(ob->data);
    if (!mesh) {
      return;
    }
    const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);
    BMeshCreateParams create_params{};
    BMeshFromMeshParams convert_params{};
    convert_params.calc_face_normal = true;
    convert_params.calc_vert_normal = true;
    bm = BM_mesh_create(&allocsize, &create_params);
    BM_mesh_bm_from_me(bm, mesh, &convert_params);
    owns_bm = true;
  }

  const BMUVOffsets offsets = image_paint_selection_uv_offsets_get(bm, ob, scene);
  if (offsets.uv < 0) {
    if (owns_bm) {
      BM_mesh_free(bm);
    }
    return;
  }

  Vector<int> seed_faces;
  seed_faces.reserve(bm->totface);
  /* O(1) per-face dedup guard -- avoids quadratic append_non_duplicates in tile loops. */
  Array<bool> face_seen(bm->totface, false);

  if (gesture_uv_bounds != nullptr) {
    BMIter fiter;
    BMFace *efa;
    int face_index;
    BM_ITER_MESH_INDEX (efa, &fiter, bm, BM_FACES_OF_MESH, face_index) {
      if (BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
        continue;
      }
      if (face_uv_intersects_rect(efa, offsets, *gesture_uv_bounds)) {
        face_seen[face_index] = true;
        seed_faces.append(face_index);
      }
    }
  }
  else {
    /* Fallback: seed from faces overlapping already-selected mask pixels. */
    for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
      const float2 uv_origin = image_select_udim_tile_uv_origin(tile->tile_number);
      /* Seeding only reads the mask, so take the `const Image *` overload. */
      const ImBuf *mask = BKE_image_paint_selection_mask_lookup(const_cast<const Image *>(image),
                                                                tile->tile_number);
      if (!mask) {
        continue;
      }
      BMIter fiter;
      BMFace *efa;
      int face_index;
      BM_ITER_MESH_INDEX (efa, &fiter, bm, BM_FACES_OF_MESH, face_index) {
        if (BM_elem_flag_test(efa, BM_ELEM_HIDDEN) || face_seen[face_index]) {
          continue;
        }
        if (face_uv_tri_intersects_mask(efa, offsets, uv_origin, mask, threshold)) {
          face_seen[face_index] = true;
          seed_faces.append(face_index);
        }
      }
    }
  }

  if (seed_faces.is_empty()) {
    if (owns_bm) {
      BM_mesh_free(bm);
    }
    return;
  }

  Array<bool> faces_to_write(bm->totface, false);
  if (expand_islands) {
    ED_uvedit_uv_islands_tag_from_face_indices(scene, bm, offsets, seed_faces, 0, faces_to_write);
  }
  else {
    for (const int face_index : seed_faces) {
      faces_to_write[face_index] = true;
    }
  }

  /* Bucket selected faces by the UDIM tile(s) their UVs fall on, in a single pass over the mesh.
   * This avoids re-scanning every face once per tile during rasterization (was O(tiles * faces)).
   * A face straddling a tile border is registered with every tile it overlaps; the rasterizer
   * clips to each tile's pixel bounds, so per-tile clipping stays correct. */
  Map<int, Vector<int>> tile_faces;
  {
    BMIter fiter;
    BMFace *efa;
    int face_index;
    BM_ITER_MESH_INDEX (efa, &fiter, bm, BM_FACES_OF_MESH, face_index) {
      if (!faces_to_write[face_index]) {
        continue;
      }
      rctf face_uv_bounds;
      BLI_rctf_init_minmax(&face_uv_bounds);
      BMIter liter;
      BMLoop *l;
      BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
        const float *uv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
        BLI_rctf_do_minmax_v(&face_uv_bounds, uv);
      }
      const int tx_min = int(floorf(face_uv_bounds.xmin));
      const int tx_max = int(floorf(face_uv_bounds.xmax));
      const int ty_min = int(floorf(face_uv_bounds.ymin));
      const int ty_max = int(floorf(face_uv_bounds.ymax));
      for (int ty = ty_min; ty <= ty_max; ty++) {
        if (ty < 0) {
          continue;
        }
        for (int tx = tx_min; tx <= tx_max; tx++) {
          /* UDIM tiles span columns 0..9; ignore UVs outside the valid grid. */
          if (tx < 0 || tx > 9) {
            continue;
          }
          const int tile_number = 1001 + ty * 10 + tx;
          if (tile_number > IMA_UDIM_MAX) {
            continue;
          }
          tile_faces.lookup_or_add_default(tile_number).append(face_index);
        }
      }
    }
  }

  /* `BM_face_at_index` below relies on the face table the tag step already ensured. */
  BM_mesh_elem_table_ensure(bm, BM_FACE);

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    const Vector<int> *faces = tile_faces.lookup_ptr(tile->tile_number);
    if (faces == nullptr) {
      continue;
    }
    const float2 uv_origin = image_select_udim_tile_uv_origin(tile->tile_number);

    ImBuf *mask;
    if (fill_value < 0.5f) {
      /* Subtract mode: only write to tiles that already have a selection mask.
       * Avoids allocating empty masks on tiles that were never selected. */
      mask = BKE_image_paint_selection_mask_lookup(image, tile->tile_number);
      if (!mask) {
        continue;
      }
    }
    else {
      ImageUser iuser{};
      iuser.tile = tile->tile_number;
      ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
      if (!ibuf) {
        continue;
      }
      mask = BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);
      BKE_image_release_ibuf(image, ibuf, nullptr);
    }

    for (const int face_index : *faces) {
      BMFace *efa = BM_face_at_index(bm, face_index);
      rasterize_face_to_mask(efa, offsets, uv_origin, mask, fill_value);
    }
  }

  if (owns_bm) {
    BM_mesh_free(bm);
  }
}

/**
 * Rebuild the image's runtime face-selection-derived 2D paint masks from the face selection
 * (#Mesh.editflag & #ME_EDIT_PAINT_FACE_SEL + `.select_poly`) of every canvas object, rasterized
 * through each object's active paint-canvas UV map. Called when 2D painting sessions begin so
 * Image Editor strokes, fills and curve patches respect the same face selection the 3D viewport
 * does while the masking is enabled.
 *
 * The derived masks live in #ImageRuntime::paint_selection_face_masks, strictly separate from the
 * user-authored masks: the sync never touches those, and the two are combined at sampling time
 * (#BKE_image_paint_selection_blend_sample). The derived state is Active only when at least one
 * canvas object has an active selection; every other object rasterizes all its visible faces, so
 * an object that is masked off in 3D is unrestricted in 2D rather than blocked. With no active
 * selection anywhere (or no UV map to rasterize through) the derived state is Inactive and the
 * masks are freed.
 *
 * A cache key over every contributing object's mesh UID, selection contents and UV map keeps
 * unchanged selections from rebuilding anything: the common stroke start costs one hash pass
 * instead of a full BMesh conversion, rasterization and blend-mask invalidation.
 *
 * Bucketing walks the UDIM grid (tiles `1001 + ty*10 + tx`, `tx` 0..9): UVs outside the valid
 * grid are ignored, matching the tiled canvas model.
 */
void image_paint_selection_mask_from_face_selection(const bContext *C,
                                                    const Scene *scene,
                                                    Image *image)
{
  if (image == nullptr || image->runtime == nullptr || scene == nullptr) {
    return;
  }
  bke::ImageRuntime &runtime = *image->runtime;

  /* A generous candidate set (see #ImagePaintCanvasPurpose::Mask): every object that may paint
   * into this image, multi-object included. */
  const Vector<Object *> objects = image_paint_selection_canvas_objects_get(
      C, image, ImagePaintCanvasPurpose::Mask);

  /* A lightweight per-object source: the selection state and UV attributes only, without the
   * per-stroke #ed::sculpt_paint::FaceSelectionMask (whose per-vertex table the rasterization
   * never uses). */
  struct FaceSelectionSyncSource {
    const Mesh *mesh = nullptr;
    /** Mesh-level masking state. Only #FaceSelectionState::Active restricts to the selected faces;
     * every other state draws all visible faces so the 2D result matches the 3D viewport for that
     * object. */
    ed::sculpt_paint::FaceSelectionState selection_state =
        ed::sculpt_paint::FaceSelectionState::Disabled;
    /** Selection varray; invalid when the mesh has no `.select_poly` (empty/disabled). */
    VArray<bool> select_poly_varray;
    /** The object's paint-canvas UVs (corner domain); invalid when there is no UV map to
     * rasterize through. */
    VArray<float2> uvs_varray;
    VArraySpan<float2> uvs;
  };

  /* Determine whether any canvas object has an active selection at all (with a UV map to rasterize
   * it through). If not, the derived state is Inactive and nothing is hashed or rasterized -- the
   * common "masking off" stroke start. */
  bool any_active = false;
  for (Object *ob : objects) {
    if (ob == nullptr || ob->type != OB_MESH || ob->data == nullptr) {
      continue;
    }
    const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
    if (ed::sculpt_paint::face_selection_state(mesh) !=
        ed::sculpt_paint::FaceSelectionState::Active)
    {
      continue;
    }
    if (image_paint_canvas_mesh_uv_name_get(*scene, *ob, mesh)) {
      any_active = true;
      break;
    }
  }
  if (!any_active) {
    if (runtime.paint_selection_derived_active ||
        !runtime.paint_selection_derived_sharing_infos.is_empty())
    {
      /* The masking was turned off (or no active selection is left): drop the derived masks -- and
       * only those, the user-authored masks are never touched -- so 2D painting returns to the
       * unmasked, user-driven state. Also releases the held sharing infos. */
      BKE_image_paint_selection_face_mask_free(image);
    }
    runtime.paint_selection_derived_active = false;
    return;
  }

  /* Deterministic chunked hashing so the per-object key inputs can combine in parallel: every item
   * is hashed together with its index and the chunk hashes XOR together, which makes the result
   * independent of how the range is split across threads. The previous scheme seeded each chunk
   * with `range.first()`, so the same data could produce different keys depending on the
   * scheduling and force spurious rebuilds. */
  const auto hash_chunks = [](const int64_t size, auto item_hash) -> uint64_t {
    return threading::parallel_reduce(
        IndexRange(size),
        2048,
        uint64_t(0),
        [&](const IndexRange range, const uint64_t /*ident*/) {
          uint64_t chunk = 0;
          for (const int64_t i : range) {
            chunk ^= item_hash(i);
          }
          return chunk;
        },
        std::bit_xor<uint64_t>());
  };

  /* An attribute's contribution to the key. Shared attribute data is identified by its sharing
   * info and #ImplicitSharingInfo::version, which every write through an attribute writer bumps:
   * O(1) instead of hashing every face and corner on each stroke start. The infos are kept alive
   * with weak users once the key is stored (see #ImageRuntime::paint_selection_derived_sharing_infos),
   * so a matching address always means the same attribute. Data without a sharing info falls back
   * to hashing its contents. */
  Vector<const ImplicitSharingInfo *> sharing_infos;
  const auto attribute_key = [&](const ImplicitSharingInfo *info,
                                 const FunctionRef<uint64_t()> content_hash) -> uint64_t {
    if (info == nullptr) {
      return content_hash();
    }
    sharing_infos.append(info);
    return get_default_hash(uint64_t(uintptr_t(info)), uint64_t(info->version()));
  };

  Vector<FaceSelectionSyncSource> sources;
  /* The same Mesh can back several canvas objects (linked duplicates, Alt+D), and its selection and
   * UVs are identical for all of them. Deduplicating by (mesh, UV map) keeps every physical
   * contribution in the key exactly once: the previous plain XOR let a duplicated mesh cancel its
   * own contribution, freezing the key and leaving stale derived masks. */
  VectorSet<std::pair<const Mesh *, std::string>> seen_sources;
  uint64_t sync_key = 0;
  for (Object *ob : objects) {
    if (ob == nullptr || ob->type != OB_MESH || ob->data == nullptr) {
      continue;
    }
    const Mesh &mesh = *id_cast<const Mesh *>(ob->data);

    /* Resolve the UV map the paint canvas is sampled through (material slot override first),
     * matching #image_paint_selection_uv_offsets_get's BMesh-based resolution directly on the
     * mesh. */
    const std::optional<std::string> uv_name = image_paint_canvas_mesh_uv_name_get(
        *scene, *ob, mesh);
    const std::pair<const Mesh *, std::string> source_key{&mesh,
                                                          uv_name.value_or(std::string())};
    if (!seen_sources.add(source_key)) {
      continue;
    }

    FaceSelectionSyncSource source;
    source.mesh = &mesh;
    source.selection_state = ed::sculpt_paint::face_selection_state(mesh);
    const bke::AttributeReader<bool> select_poly_reader = mesh.attributes().lookup<bool>(
        ".select_poly", bke::AttrDomain::Face);
    source.select_poly_varray = select_poly_reader.varray;
    bke::AttributeReader<float2> uvs_reader;
    if (uv_name && !uv_name->empty()) {
      uvs_reader = mesh.attributes().lookup<float2>(*uv_name, bke::AttrDomain::Corner);
      source.uvs_varray = uvs_reader.varray;
      if (source.uvs_varray) {
        source.uvs = VArraySpan<float2>(source.uvs_varray);
      }
    }

    uint64_t poly_hash = 0;
    uint64_t uv_hash = 0;
    /* Only an active selection feeds the key: a non-active source draws all its faces regardless
     * of `.select_poly`, so hashing the selection there would force spurious rebuilds. */
    if (source.selection_state == ed::sculpt_paint::FaceSelectionState::Active &&
        source.select_poly_varray)
    {
      const VArray<bool> &select_poly = source.select_poly_varray;
      poly_hash = attribute_key(select_poly_reader.sharing_info, [&]() {
        return hash_chunks(mesh.faces_num, [&](const int64_t i) {
          return get_default_hash(uint64_t(i), select_poly[int(i)]);
        });
      });
    }
    if (source.uvs_varray) {
      /* Re-unwrapping or editing the active map must invalidate the derived masks even when the
       * selection itself didn't change. */
      const VArray<float2> &uvs = source.uvs_varray;
      uv_hash = attribute_key(uvs_reader.sharing_info, [&]() {
        return hash_chunks(mesh.corners_num, [&](const int64_t i) {
          const float2 &uv = uvs[int(i)];
          return get_default_hash(uint64_t(i), uv.x, uv.y);
        });
      });
    }
    /* Sequential combine in iteration order: unlike the previous XOR this cannot cancel two
     * contributions, and the dedup above makes the set of contributions unique. Mesh identity, the
     * selection state and contents, the UV map name AND contents all feed the key, so any of them
     * changing forces a rebuild. #get_default_hash takes at most 6 values, so the key is folded
     * pairwise like #draw::Manager and #BKE_viewer_path do. */
    sync_key = get_default_hash(sync_key, mesh.id.session_uid);
    sync_key = get_default_hash(sync_key, mesh.faces_num);
    sync_key = get_default_hash(sync_key, mesh.corners_num);
    sync_key = get_default_hash(sync_key, int(source.selection_state));
    sync_key = get_default_hash(sync_key, poly_hash);
    sync_key = get_default_hash(sync_key, uv_hash);
    sync_key = get_default_hash(sync_key, get_default_hash(uv_name.value_or(std::string())));
    sources.append(std::move(source));
  }

  /* The image's tile set and resolutions feed the key too: adding or removing a tile, or resizing
   * the image, must rebuild the derived masks (they are per-tile, per-resolution). The ibufs are
   * cached, so probing them here is cheap. */
  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser{};
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    const int2 size = ibuf ? int2(ibuf->x, ibuf->y) : int2(0);
    if (ibuf) {
      BKE_image_release_ibuf(image, ibuf, nullptr);
    }
    sync_key ^= get_default_hash(uint(tile->tile_number), size.x, size.y);
  }

  if (runtime.paint_selection_derived_sync_key == sync_key) {
    /* The selection and every other key input are unchanged since the derived masks were built:
     * keep them. No free, no rasterization, no blend-mask invalidation this stroke. */
    return;
  }

  /* Rebuild the derived masks from scratch. #BKE_image_paint_selection_face_mask_free resets the
   * sync key, so set it after the rebuild starts. */
  BKE_image_paint_selection_face_mask_free(image);
  runtime.paint_selection_derived_sync_key = sync_key;
  runtime.paint_selection_derived_active = true;
  for (const ImplicitSharingInfo *info : sharing_infos) {
    info->add_weak_user();
  }
  runtime.paint_selection_derived_sharing_infos = std::move(sharing_infos);

  /* Bucket selected faces by the UDIM tile(s) their UVs fall on; a face straddling a tile border
   * is registered with every tile it overlaps (the rasterizer clips per tile). Face indices are
   * per source. */
  Map<int, Vector<std::pair<int, int>>> tile_faces;
  for (const int src_i : sources.index_range()) {
    const FaceSelectionSyncSource &source = sources[src_i];
    if (!source.uvs_varray) {
      /* No UV map to rasterize through: this object contributes nothing to the derived masks. */
      continue;
    }
    /* An active source draws only its selected faces; a disabled or empty one draws all visible
     * faces, so painting it in 2D matches its unrestricted 3D behavior. */
    const bool draw_all = source.selection_state != ed::sculpt_paint::FaceSelectionState::Active;
    const VArraySpan<bool> select_poly(source.select_poly_varray);
    const OffsetIndices<int> faces = source.mesh->faces();
    const VArraySpan<bool> hide_poly = *source.mesh->attributes().lookup<bool>(".hide_poly",
                                                                              bke::AttrDomain::Face);
    for (const int face : faces.index_range()) {
      if (!draw_all && (select_poly.is_empty() || !select_poly[face])) {
        continue;
      }
      if (!hide_poly.is_empty() && hide_poly[face]) {
        continue;
      }
      rctf face_uv_bounds;
      BLI_rctf_init_minmax(&face_uv_bounds);
      for (const int corner : faces[face]) {
        BLI_rctf_do_minmax_v(&face_uv_bounds, source.uvs[corner]);
      }
      const int tx_min = int(floorf(face_uv_bounds.xmin));
      const int tx_max = int(floorf(face_uv_bounds.xmax));
      const int ty_min = int(floorf(face_uv_bounds.ymin));
      const int ty_max = int(floorf(face_uv_bounds.ymax));
      for (int ty = ty_min; ty <= ty_max; ty++) {
        if (ty < 0) {
          continue;
        }
        for (int tx = tx_min; tx <= tx_max; tx++) {
          /* UDIM tiles span columns 0..9; ignore UVs outside the valid grid. */
          if (tx < 0 || tx > 9) {
            continue;
          }
          const int tile_number = 1001 + ty * 10 + tx;
          if (tile_number > IMA_UDIM_MAX) {
            continue;
          }
          tile_faces.lookup_or_add_default(tile_number).append({src_i, face});
        }
      }
    }
  }

  struct FaceSelectionTileJob {
    int tile_number;
    ImBuf *tile_mask = nullptr;
    Vector<std::pair<int, int>> faces;
  };
  Vector<FaceSelectionTileJob> jobs;
  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    const Vector<std::pair<int, int>> *faces = tile_faces.lookup_ptr(tile->tile_number);
    if (faces == nullptr) {
      continue;
    }
    FaceSelectionTileJob job;
    job.tile_number = tile->tile_number;
    job.faces = *faces;
    ImageUser iuser{};
    iuser.tile = job.tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (ibuf) {
      job.tile_mask = BKE_image_paint_selection_face_mask_get(
          image, job.tile_number, ibuf->x, ibuf->y);
      BKE_image_release_ibuf(image, ibuf, nullptr);
    }
    jobs.append(std::move(job));
  }

  const bool any_tile_mask = std::any_of(jobs.begin(), jobs.end(), [](const FaceSelectionTileJob &job) {
    return job.tile_mask != nullptr;
  });

  /* Each tile writes its own mask buffer, so tiles fill in parallel; the masks (and the map
   * holding them) were created serially above. */
  threading::parallel_for(jobs.index_range(), 1, [&](const IndexRange range) {
    for (const int j : range) {
      FaceSelectionTileJob &job = jobs[j];
      if (job.tile_mask == nullptr) {
        continue;
      }
      float *data = job.tile_mask->float_data_for_write();
      const int width = job.tile_mask->x;
      const int height = job.tile_mask->y;
      const float2 uv_origin = image_select_udim_tile_uv_origin(job.tile_number);
      for (const auto &[src_i, face] : job.faces) {
        const FaceSelectionSyncSource &source = sources[src_i];
        Vector<float2, 8> px_verts;
        const OffsetIndices<int> faces = source.mesh->faces();
        for (const int corner : faces[face]) {
          const float2 uv = source.uvs[corner];
          px_verts.append(
              float2((uv.x - uv_origin.x) * width, (uv.y - uv_origin.y) * height));
        }
        foreach_uv_polygon_pixel(
            px_verts, width, height, [&](const int x, const int y, const bool /*strict*/) {
              data[size_t(y) * width + x] = 1.0f;
              return true;
            });
      }
    }
  });
  /* Nothing could be rasterized (e.g. the image has no tiles): the derived state is inactive. */
  runtime.paint_selection_derived_active = any_tile_mask;
}

void image_paint_selection_expand(bContext *C,
                                  Image *image,
                                  const eSelectOp sel_op,
                                  const rctf *gesture_uv_bounds)
{
  Scene *scene = CTX_data_scene(C);
  const char expand = scene->toolsettings->imapaint.selection_expand;
  if (!ELEM(expand, IMAGE_PAINT_SELECT_EXPAND_FACE, IMAGE_PAINT_SELECT_EXPAND_ISLAND)) {
    return;
  }
  if (sel_op == SEL_OP_SUB && !BKE_image_paint_selection_mask_has_any(image)) {
    return;
  }

  const float fill_value = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;
  const float threshold = SELECTION_MASK_THRESHOLD;
  const bool expand_islands = (expand == IMAGE_PAINT_SELECT_EXPAND_ISLAND);

  Vector<Object *> objects = image_paint_selection_canvas_objects_get(
      C, image, ImagePaintCanvasPurpose::Mask);

  for (Object *ob : objects) {
    image_paint_selection_expand_for_object(
        scene, ob, image, fill_value, threshold, gesture_uv_bounds, expand_islands);
  }
}

/** \} */
/* -------------------------------------------------------------------- */
/** \name Select All
 * \{ */

static wmOperatorStatus image_select_all_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  /* End any floating session before changing the selection state, so its undo step is closed
   * before "Select All" opens its own with #ED_image_undo_push_begin_selection. This used to
   * commit only a floating *move*, leaving a floating transform / warp holding a step that the
   * push below then freed under it -- the same dangling-step crash
   * #image_select_floating_sessions_end exists to prevent. */
  image_select_floating_sessions_end_all(C, sima);

  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  const Vector<ImagePaintSelectionTarget> targets = image_paint_selection_targets_get(C, sima);

  /* Resolve the image editor's active UDIM once, then apply that same UV tile to every PBR target.
   * This exactly preserves single-image behavior and avoids each target silently selecting its own
   * unrelated active tile. */
  ImageUser active_iuser = sima->iuser;
  if (image->source == IMA_SRC_TILED) {
    const ImageTile *active = static_cast<const ImageTile *>(
        BLI_findlink(&image->tiles, image->active_tile_index));
    if (!active) {
      active = static_cast<const ImageTile *>(image->tiles.first);
    }
    if (!active) {
      return OPERATOR_CANCELLED;
    }
    active_iuser.tile = active->tile_number;
  }
  const ImageTile *active_tile = BKE_image_get_tile_from_iuser(image, &active_iuser);
  if (!active_tile) {
    return OPERATOR_CANCELLED;
  }
  const int active_tile_number = active_tile->tile_number;

  image_paint_selection_undo_begin("Select All", targets);
  for (const ImagePaintSelectionTarget &target : targets) {
    /* In Material canvas mode all targets receive the same active UDIM. A target without that tile
     * is skipped, rather than selecting an unrelated tile selected by its own active index. */
    ImageUser iuser = target.iuser;
    iuser.tile = active_tile_number;
    const ImageTile *target_tile = BKE_image_get_tile_from_iuser(target.image, &iuser);
    if (!target_tile) {
      continue;
    }
    iuser.tile = target_tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(target.image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }
    BKE_image_paint_selection_mask_get(target.image, target_tile->tile_number, ibuf->x, ibuf->y);
    BKE_image_paint_selection_mask_fill(target.image, target_tile->tile_number, 1.0f);
    BKE_image_paint_selection_edge_policy_set(
        target.image, BKE_image_paint_selection_edge_policy_hard());
    BKE_image_release_ibuf(target.image, ibuf, nullptr);
  }

  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_all(wmOperatorType *ot)
{
  ot->name = "Select All";
  ot->idname = "PAINT_OT_image_select_all";
  ot->description = "Select the entire image as a paint mask";
  ot->exec = image_select_all_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select None
 * \{ */

static wmOperatorStatus image_select_none_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  /* End any floating session before changing the selection state, so its undo step is closed
   * before "Select None" opens its own. See #image_select_all_exec. */
  image_select_floating_sessions_end_all(C, sima);

  const Vector<ImagePaintSelectionTarget> targets = image_paint_selection_targets_get(C, sima);
  image_paint_selection_undo_begin("Select None", targets);
  for (const ImagePaintSelectionTarget &target : targets) {
    BKE_image_paint_selection_mask_free(target.image);
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_none(wmOperatorType *ot)
{
  ot->name = "Select None";
  ot->idname = "PAINT_OT_image_select_none";
  ot->description = "Deselect the entire image (remove paint mask)";
  ot->exec = image_select_none_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invert Selection
 * \{ */

static wmOperatorStatus image_select_invert_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  /* End any floating session before changing the selection state, so its undo step is closed
   * before "Invert Selection" opens its own. See #image_select_all_exec. */
  image_select_floating_sessions_end_all(C, sima);

  const Vector<ImagePaintSelectionTarget> targets = image_paint_selection_targets_get(C, sima);
  image_paint_selection_undo_begin("Invert Selection", targets);

  for (const ImagePaintSelectionTarget &target : targets) {
    for (ImageTile *tile : ListBaseWrapper<ImageTile>(target.image->tiles)) {
      ImageUser iuser = target.iuser;
      iuser.tile = tile->tile_number;
      ImBuf *ibuf = BKE_image_acquire_ibuf(target.image, &iuser, nullptr);
      if (!ibuf) {
        continue;
      }

      BKE_image_paint_selection_mask_get(target.image, tile->tile_number, ibuf->x, ibuf->y);
      BKE_image_paint_selection_mask_invert(target.image, tile->tile_number);

      BKE_image_release_ibuf(target.image, ibuf, nullptr);
    }
  }

  image_paint_selection_targets_update(C, targets);

  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_invert(wmOperatorType *ot)
{
  ot->name = "Invert Selection";
  ot->idname = "PAINT_OT_image_select_invert";
  ot->description = "Invert the current paint selection mask";
  ot->exec = image_select_invert_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Box
 * \{ */

/* Shared with Lasso / Polyline below. The mirror flags rasterize the polygon across the tile's
 * own centerlines, in tile-pixel space (`x' = width - x`); see
 * #ImageSelectGestureShape::rasterize_tile_mirrored. Both implementations live in
 * paint_image_select_gesture.cc next to the shared gesture sequence. */

/**
 * Rectangular screen-space gesture. Its UV projection is an axis-aligned rectangle only while the
 * canvas is unrotated; with canvas rotation it is a quadrilateral and must be rasterized as one.
 */
class ImageSelectBoxShape : public ImageSelectGestureShape {
  Vector<float2> uv_points_;

 public:
  const char *undo_name() const override
  {
    return "Box Select";
  }

  bool uv_bounds_calc(const ARegion *region, wmOperator *op, rctf &r_uv_bounds) override
  {
    rctf rectf;
    WM_operator_properties_border_to_rctf(op, &rectf);
    if (BLI_rctf_size_x(&rectf) == 0.0f && BLI_rctf_size_y(&rectf) == 0.0f) {
      return false;
    }

    uv_points_.reinitialize(4);
    const float region_points[4][2] = {{rectf.xmin, rectf.ymin},
                                       {rectf.xmax, rectf.ymin},
                                       {rectf.xmax, rectf.ymax},
                                       {rectf.xmin, rectf.ymax}};
    BLI_rctf_init_minmax(&r_uv_bounds);
    for (const int i : IndexRange(4)) {
      float uv[2];
      ui::view2d_region_to_view(
          &region->v2d, region_points[i][0], region_points[i][1], &uv[0], &uv[1]);
      uv_points_[i] = float2(uv[0], uv[1]);
      BLI_rctf_do_minmax_v(&r_uv_bounds, uv);
    }

    if (region->v2d.rotation == 0.0f) {
      /* Preserve the existing axis-aligned UDIM-border snap where it is well-defined. Rotating a
       * screen-space box produces a UV quadrilateral, whose edges cannot independently snap to
       * horizontal/vertical UDIM borders without changing the gesture's shape. */
      const float snap_thresh = 0.02f;
      auto snap_to_udim_border = [](float v, const float threshold) -> float {
        const float rounded = roundf(v);
        return (fabsf(v - rounded) < threshold) ? rounded : v;
      };
      r_uv_bounds.xmin = snap_to_udim_border(r_uv_bounds.xmin, snap_thresh);
      r_uv_bounds.xmax = snap_to_udim_border(r_uv_bounds.xmax, snap_thresh);
      r_uv_bounds.ymin = snap_to_udim_border(r_uv_bounds.ymin, snap_thresh);
      r_uv_bounds.ymax = snap_to_udim_border(r_uv_bounds.ymax, snap_thresh);
      uv_points_[0] = float2(r_uv_bounds.xmin, r_uv_bounds.ymin);
      uv_points_[1] = float2(r_uv_bounds.xmax, r_uv_bounds.ymin);
      uv_points_[2] = float2(r_uv_bounds.xmax, r_uv_bounds.ymax);
      uv_points_[3] = float2(r_uv_bounds.xmin, r_uv_bounds.ymax);
    }
    return true;
  }

  void rasterize_tile(const float2 &uv_origin,
                      const rctf & /*tile_uv_rect*/,
                      ImBuf *mask,
                      const float fill_value) const override
  {
    image_select_uv_polygon_rasterize_tile(uv_origin, uv_points_, mask, fill_value);
  }

  void rasterize_tile_mirrored(const float2 &uv_origin,
                               const rctf & /*tile_uv_rect*/,
                               ImBuf *mask,
                               const float fill_value,
                               const bool mirror_x,
                               const bool mirror_y) const override
  {
    image_select_uv_polygon_rasterize_tile(
        uv_origin, uv_points_, mask, fill_value, mirror_x, mirror_y);
  }

  Vector<float2> uv_outline() const override
  {
    return uv_points_;
  }

  PaintSelectionEdgePolicy edge_policy() const override
  {
    return BKE_image_paint_selection_edge_policy_hard();
  }
};

static wmOperatorStatus image_select_box_exec(bContext *C, wmOperator *op)
{
  ImageSelectBoxShape shape;
  return image_select_gesture_exec_generic(C, op, shape);
}

/**
 * Live header hint: the gesture's width and height, in the active tile's canvas pixels. Cleared
 * by the modal when the gesture ends.
 */
static void image_select_box_status_update(bContext *C, wmOperator *op, const wmEvent *event)
{
  const ARegion *region = CTX_wm_region(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  ScrArea *area = CTX_wm_area(C);
  if (!region || !sima || !sima->image || !area) {
    return;
  }

  float uv_a[2], uv_b[2];
  ui::view2d_region_to_view(&region->v2d,
                            float(RNA_int_get(op->ptr, "click_x")),
                            float(RNA_int_get(op->ptr, "click_y")),
                            &uv_a[0],
                            &uv_a[1]);
  ui::view2d_region_to_view(
      &region->v2d, float(event->mval[0]), float(event->mval[1]), &uv_b[0], &uv_b[1]);

  ImageUser iuser = sima->iuser;
  ImBuf *ibuf = BKE_image_acquire_ibuf(sima->image, &iuser, nullptr);
  if (!ibuf) {
    return;
  }
  const int width_px = int(std::fabs(uv_b[0] - uv_a[0]) * float(ibuf->x));
  const int height_px = int(std::fabs(uv_b[1] - uv_a[1]) * float(ibuf->y));
  BKE_image_release_ibuf(sima->image, ibuf, nullptr);

  char text[64];
  SNPRINTF(text, "Width: %d  Height: %d", width_px, height_px);
  ED_area_status_text(area, text);
}

static void image_select_status_clear(bContext *C)
{
  if (ScrArea *area = CTX_wm_area(C)) {
    ED_area_status_text(area, nullptr);
  }
}

static wmOperatorStatus image_select_box_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (image_select_gesture_invoke_begin(C, op, event)) {
    return OPERATOR_FINISHED;
  }
  return WM_gesture_box_invoke(C, op, event);
}

static wmOperatorStatus image_select_box_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  image_select_gesture_drag_detect(op, event);
  const wmOperatorStatus ret = WM_gesture_box_modal(C, op, event);
  if (event->type == MOUSEMOVE && (ret & OPERATOR_RUNNING_MODAL)) {
    image_select_box_status_update(C, op, event);
  }
  else if ((ret & OPERATOR_RUNNING_MODAL) == 0) {
    image_select_status_clear(C);
  }
  return ret;
}

/**
 * Flags shared by all three gesture selection operators.
 *
 * No OPTYPE_UNDO: these operators open and close their own image undo step with
 * #ED_image_undo_push_begin_selection / #ED_image_undo_push_end inside `exec`. Letting WM push a
 * second step on top would duplicate every selection change in the undo stack. This follows the
 * convention already documented on #PAINT_OT_image_select_move for the rest of this feature, and
 * deliberately differs from #UV_OT_select_box / #VIEW3D_OT_select_box, which set OPTYPE_UNDO
 * precisely because they have no undo step of their own.
 *
 * No OPTYPE_BLOCKING either: no gesture selection operator in the editors tree sets it, and the
 * modal only runs for the duration of a single LMB drag (see #PAINT_OT_image_select_move).
 *
 * OPTYPE_REGISTER is kept so the "Adjust Last Operation" panel can still tweak `mode`; the
 * transient click-tracking properties are hidden from it via PROP_HIDDEN | PROP_SKIP_SAVE in
 * #image_select_gesture_properties.
 */
static constexpr short IMAGE_SELECT_GESTURE_OPTYPE_FLAGS = OPTYPE_REGISTER;

void PAINT_OT_image_select_box(wmOperatorType *ot)
{
  ot->name = "Select Box";
  ot->idname = "PAINT_OT_image_select_box";
  ot->description = "Select a rectangular region as a paint mask";

  ot->invoke = image_select_box_invoke;
  ot->modal = image_select_box_modal;
  ot->exec = image_select_box_exec;
  ot->cancel = WM_gesture_box_cancel;
  ot->poll = image_paint_selection_poll;
  ot->flag = IMAGE_SELECT_GESTURE_OPTYPE_FLAGS;

  WM_operator_properties_gesture_box(ot);
  WM_operator_properties_select_operation_simple(ot);

  image_select_gesture_properties(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Lasso
 * \{ */

/**
 * Convert the operator's `path` (region pixels, same RNA as lasso and polyline)
 * into UV-space points and the axis-aligned UV bounds of that polygon.
 * \return false when there are fewer than three points (degenerate gesture).
 */
static bool image_select_path_to_uv_points(bContext *C,
                                           const ARegion *region,
                                           wmOperator *op,
                                           Vector<float2> &r_uv_points,
                                           rctf &r_uv_bounds)
{
  const Array<int2> mcoords = WM_gesture_lasso_path_to_array(C, op);
  if (mcoords.size() < 3) {
    return false;
  }

  r_uv_points.clear();
  r_uv_points.reserve(mcoords.size());
  BLI_rctf_init_minmax(&r_uv_bounds);
  for (const int2 &p : mcoords) {
    float co[2] = {float(p.x), float(p.y)};
    ui::view2d_region_to_view(&region->v2d, co[0], co[1], &co[0], &co[1]);
    r_uv_points.append(float2(co[0], co[1]));
    BLI_rctf_do_minmax_v(&r_uv_bounds, co);
  }
  return true;
}

/** Freehand gesture; caches its UV-space outline for per-tile rasterization. */
class ImageSelectLassoShape : public ImageSelectGestureShape {
  bContext *C_;
  Vector<float2> uv_points_;

 public:
  explicit ImageSelectLassoShape(bContext *C) : C_(C) {}

  const char *undo_name() const override
  {
    return "Lasso Select";
  }

  bool uv_bounds_calc(const ARegion *region, wmOperator *op, rctf &r_uv_bounds) override
  {
    return image_select_path_to_uv_points(C_, region, op, uv_points_, r_uv_bounds);
  }

  void rasterize_tile(const float2 &uv_origin,
                      const rctf & /*tile_uv_rect*/,
                      ImBuf *mask,
                      const float fill_value) const override
  {
    image_select_uv_polygon_rasterize_tile(uv_origin, uv_points_, mask, fill_value);
  }

  void rasterize_tile_mirrored(const float2 &uv_origin,
                               const rctf & /*tile_uv_rect*/,
                               ImBuf *mask,
                               const float fill_value,
                               const bool mirror_x,
                               const bool mirror_y) const override
  {
    image_select_uv_polygon_rasterize_tile(
        uv_origin, uv_points_, mask, fill_value, mirror_x, mirror_y);
  }

  Vector<float2> uv_outline() const override
  {
    return uv_points_;
  }

  PaintSelectionEdgePolicy edge_policy() const override
  {
    return BKE_image_paint_selection_edge_policy_feathered();
  }
};

static wmOperatorStatus image_select_lasso_exec(bContext *C, wmOperator *op)
{
  ImageSelectLassoShape shape(C);
  return image_select_gesture_exec_generic(C, op, shape);
}

static wmOperatorStatus image_select_lasso_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  if (image_select_gesture_invoke_begin(C, op, event)) {
    return OPERATOR_FINISHED;
  }
  return WM_gesture_lasso_invoke(C, op, event);
}

static wmOperatorStatus image_select_lasso_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  image_select_gesture_drag_detect(op, event);
  return WM_gesture_lasso_modal(C, op, event);
}

void PAINT_OT_image_select_lasso(wmOperatorType *ot)
{
  ot->name = "Select Lasso";
  ot->idname = "PAINT_OT_image_select_lasso";
  ot->description = "Select a freehand region as a paint mask";

  ot->invoke = image_select_lasso_invoke;
  ot->modal = image_select_lasso_modal;
  ot->exec = image_select_lasso_exec;
  /* Without a cancel callback the wmGesture held in `op->customdata` leaks when the gesture is
   * aborted; every other lasso operator in the tree installs this same handler. */
  ot->cancel = WM_gesture_lasso_cancel;
  ot->poll = image_paint_selection_poll;
  ot->flag = IMAGE_SELECT_GESTURE_OPTYPE_FLAGS;

  WM_operator_properties_gesture_lasso(ot);
  WM_operator_properties_select_operation_simple(ot);

  image_select_gesture_properties(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Polyline
 * \{ */

/**
 * Click-placed polygon; same rasterization as lasso. The WM polyline gesture
 * writes the same `path` collection, so #image_select_path_to_uv_points works
 * unchanged. Unlike box/lasso/circle this operator does not participate in
 * simple-click deselect: the first press starts placing vertices.
 */
class ImageSelectPolylineShape : public ImageSelectGestureShape {
  bContext *C_;
  Vector<float2> uv_points_;

 public:
  explicit ImageSelectPolylineShape(bContext *C) : C_(C) {}

  const char *undo_name() const override
  {
    return "Polyline Select";
  }

  bool uv_bounds_calc(const ARegion *region, wmOperator *op, rctf &r_uv_bounds) override
  {
    return image_select_path_to_uv_points(C_, region, op, uv_points_, r_uv_bounds);
  }

  void rasterize_tile(const float2 &uv_origin,
                      const rctf & /*tile_uv_rect*/,
                      ImBuf *mask,
                      const float fill_value) const override
  {
    image_select_uv_polygon_rasterize_tile(uv_origin, uv_points_, mask, fill_value);
  }

  void rasterize_tile_mirrored(const float2 &uv_origin,
                               const rctf & /*tile_uv_rect*/,
                               ImBuf *mask,
                               const float fill_value,
                               const bool mirror_x,
                               const bool mirror_y) const override
  {
    image_select_uv_polygon_rasterize_tile(
        uv_origin, uv_points_, mask, fill_value, mirror_x, mirror_y);
  }

  Vector<float2> uv_outline() const override
  {
    return uv_points_;
  }

  PaintSelectionEdgePolicy edge_policy() const override
  {
    return BKE_image_paint_selection_edge_policy_feathered();
  }
};

static wmOperatorStatus image_select_polyline_exec(bContext *C, wmOperator *op)
{
  ImageSelectPolylineShape shape(C);
  return image_select_gesture_exec_generic(C, op, shape);
}

static wmOperatorStatus image_select_polyline_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  if (image_select_move_delegate_to_move_operator(C, event)) {
    return OPERATOR_FINISHED;
  }
  return WM_gesture_polyline_invoke(C, op, event);
}

void PAINT_OT_image_select_polyline(wmOperatorType *ot)
{
  ot->name = "Select Polyline";
  ot->idname = "PAINT_OT_image_select_polyline";
  ot->description = "Select a polygonal region as a paint mask";

  ot->invoke = image_select_polyline_invoke;
  ot->modal = WM_gesture_polyline_modal;
  ot->exec = image_select_polyline_exec;
  /* Without a cancel callback the wmGesture held in `op->customdata` leaks when
   * the gesture is aborted; the four sculpt polyline operators install the same
   * handler. */
  ot->cancel = WM_gesture_polyline_cancel;
  ot->poll = image_paint_selection_poll;
  ot->flag = IMAGE_SELECT_GESTURE_OPTYPE_FLAGS;

  WM_operator_properties_gesture_polyline(ot);
  WM_operator_properties_select_operation_simple(ot);

  image_select_gesture_properties(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Circle
 * \{ */

/**
 * Rasterize a filled ellipse into a 1-channel float buffer.
 * Passing equal #rx and #ry produces a perfect circle.
 * Separate radii are needed because UV space is not always isotropic --
 * the Image Editor view2d can have a non-1:1 aspect ratio (e.g. when
 * multiple UDIM tiles are visible side-by-side), so the screen-pixel
 * radius maps to different UV extents along X and Y.
 *
 * NOTE: kept hand-rolled on purpose. blenlib's 2D drawing helpers only cover polygons and
 * triangles; none of them expresses an axis-aligned ellipse with independent X/Y radii, and
 * approximating one by a polygon would quantize the outline.
 */
static void fill_circle_float(ImBuf *ibuf, int cx, int cy, int rx, int ry, float color)
{
  const int width = ibuf->x;
  const int height = ibuf->y;
  const int x1 = max_ii(cx - rx, 0);
  const int x2 = min_ii(cx + rx, width - 1);
  const int y1 = max_ii(cy - ry, 0);
  const int y2 = min_ii(cy + ry, height - 1);
  const float rx2 = float(rx) * float(rx);
  const float ry2 = float(ry) * float(ry);

  for (int y = y1; y <= y2; y++) {
    const float dy = float(y - cy);
    const float dy2 = (dy * dy) / ry2;
    for (int x = x1; x <= x2; x++) {
      const float dx = float(x - cx);
      if ((dx * dx) / rx2 + dy2 <= 1.0f) {
        ibuf->float_data_for_write()[y * width + x] = color;
      }
    }
  }
}

/**
 * Rasterize a UV-space circle/ellipse into one tile's mask, optionally mirrored across the
 * tile's own centerlines (image-editor-space symmetry). The mirror is an affine transform of
 * the tile onto itself, so the radii carry over unchanged; only the center is flipped.
 */
static void image_select_uv_circle_rasterize_tile(const float2 &uv_origin,
                                                  const float2 &uv_center,
                                                  const float uv_radius_x,
                                                  const float uv_radius_y,
                                                  ImBuf *mask,
                                                  const float fill_value,
                                                  const bool mirror_x = false,
                                                  const bool mirror_y = false)
{
  float px = (uv_center.x - uv_origin.x) * mask->x;
  float py = (uv_center.y - uv_origin.y) * mask->y;
  if (mirror_x) {
    px = float(mask->x) - px;
  }
  if (mirror_y) {
    py = float(mask->y) - py;
  }
  const int cx = int(roundf(px));
  const int cy = int(roundf(py));
  int rx = int(roundf(uv_radius_x * mask->x));
  if (rx <= 0) {
    rx = 1;
  }
  int ry = int(roundf(uv_radius_y * mask->y));
  if (ry <= 0) {
    ry = 1;
  }
  fill_circle_float(mask, cx, cy, rx, ry, fill_value);
}

/** Circle gesture; caches its UV-space center and the two UV radii. */
class ImageSelectCircleShape : public ImageSelectGestureShape {
  float2 uv_center_;
  /* Signed along X (the view2d mapping may flip), absolute along Y -- as measured below. */
  float uv_radius_x_;
  float uv_radius_y_;

 public:
  const char *undo_name() const override
  {
    return "Circle Select";
  }

  bool uv_bounds_calc(const ARegion *region, wmOperator *op, rctf &r_uv_bounds) override
  {
    const int mradius = RNA_int_get(op->ptr, "radius");
    if (mradius <= 0) {
      return false;
    }

    const int mx = RNA_int_get(op->ptr, "x");
    const int my = RNA_int_get(op->ptr, "y");

    /* Convert center to UV space. */
    float co_center[2] = {float(mx), float(my)};
    ui::view2d_region_to_view(
        &region->v2d, co_center[0], co_center[1], &co_center[0], &co_center[1]);

    /* Convert radius to UV space along X: measure a point one radius to the right. */
    float co_edge[2] = {float(mx + mradius), float(my)};
    ui::view2d_region_to_view(&region->v2d, co_edge[0], co_edge[1], &co_edge[0], &co_edge[1]);
    const float uv_radius = co_edge[0] - co_center[0];

    /* Convert radius to UV space along Y separately.
     * The Image Editor view2d can have a non-1:1 pixel-to-UV ratio (e.g. when multiple
     * UDIM tiles are shown side-by-side), so the horizontal and vertical UV extents of
     * the same screen-pixel radius differ. Without this correction the circle appears as
     * a flattened ellipse on all UDIM tiles except the first one. */
    float co_edge_y[2] = {float(mx), float(my + mradius)};
    ui::view2d_region_to_view(
        &region->v2d, co_edge_y[0], co_edge_y[1], &co_edge_y[0], &co_edge_y[1]);
    const float uv_radius_y = fabsf(co_edge_y[1] - co_center[1]);

    uv_center_ = float2(co_center[0], co_center[1]);
    uv_radius_x_ = uv_radius;
    uv_radius_y_ = uv_radius_y;

    const float abs_uv_radius = fabsf(uv_radius);
    r_uv_bounds.xmin = co_center[0] - abs_uv_radius;
    r_uv_bounds.xmax = co_center[0] + abs_uv_radius;
    r_uv_bounds.ymin = co_center[1] - uv_radius_y;
    r_uv_bounds.ymax = co_center[1] + uv_radius_y;
    return true;
  }

  void rasterize_tile(const float2 &uv_origin,
                      const rctf & /*tile_uv_rect*/,
                      ImBuf *mask,
                      const float fill_value) const override
  {
    image_select_uv_circle_rasterize_tile(
        uv_origin, uv_center_, uv_radius_x_, uv_radius_y_, mask, fill_value);
  }

  void rasterize_tile_mirrored(const float2 &uv_origin,
                               const rctf & /*tile_uv_rect*/,
                               ImBuf *mask,
                               const float fill_value,
                               const bool mirror_x,
                               const bool mirror_y) const override
  {
    /* The helper mirrors the center in tile-pixel space, which is the exact tile-local affine
     * mirror; the radii carry over unchanged. */
    image_select_uv_circle_rasterize_tile(
        uv_origin, uv_center_, uv_radius_x_, uv_radius_y_, mask, fill_value, mirror_x, mirror_y);
  }

  Vector<float2> uv_outline() const override
  {
    /* A symmetry copy of an axis-aligned ellipse is generally rotated or bent, which the
     * analytic fill cannot express; the copies use a dense polygon (the original fill above
     * stays analytic). */
    constexpr int SAMPLES = 64;
    Vector<float2> ellipse(SAMPLES);
    for (const int i : IndexRange(SAMPLES)) {
      const float t = (2.0f * M_PI) * float(i) / float(SAMPLES);
      ellipse[i] = uv_center_ + float2(cosf(t) * uv_radius_x_, sinf(t) * uv_radius_y_);
    }
    return ellipse;
  }

  PaintSelectionEdgePolicy edge_policy() const override
  {
    return BKE_image_paint_selection_edge_policy_feathered();
  }
};

static wmOperatorStatus image_select_circle_exec(bContext *C, wmOperator *op)
{
  ImageSelectCircleShape shape;
  return image_select_gesture_exec_generic(C, op, shape);
}

static wmOperatorStatus image_select_circle_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  if (image_select_gesture_invoke_begin(C, op, event)) {
    return OPERATOR_FINISHED;
  }
  return WM_gesture_circle_invoke(C, op, event);
}

/** Live header hint: the gesture radius in the active tile's canvas pixels. */
static void image_select_circle_status_update(bContext *C, wmOperator *op)
{
  const ARegion *region = CTX_wm_region(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  ScrArea *area = CTX_wm_area(C);
  const int mradius = RNA_int_get(op->ptr, "radius");
  if (!region || !sima || !sima->image || !area || mradius <= 0) {
    return;
  }

  const int mx = RNA_int_get(op->ptr, "x");
  const int my = RNA_int_get(op->ptr, "y");
  float co_center[2] = {float(mx), float(my)};
  ui::view2d_region_to_view(&region->v2d, co_center[0], co_center[1], &co_center[0], &co_center[1]);
  float co_edge[2] = {float(mx + mradius), float(my)};
  ui::view2d_region_to_view(&region->v2d, co_edge[0], co_edge[1], &co_edge[0], &co_edge[1]);
  const float uv_radius = std::fabs(co_edge[0] - co_center[0]);

  ImageUser iuser = sima->iuser;
  ImBuf *ibuf = BKE_image_acquire_ibuf(sima->image, &iuser, nullptr);
  if (!ibuf) {
    return;
  }
  const int radius_px = int(uv_radius * float(ibuf->x));
  BKE_image_release_ibuf(sima->image, ibuf, nullptr);

  char text[64];
  SNPRINTF(text, "Radius: %d", radius_px);
  ED_area_status_text(area, text);
}

static wmOperatorStatus image_select_circle_modal(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  image_select_gesture_drag_detect(op, event);
  const wmOperatorStatus ret = WM_gesture_circle_modal(C, op, event);
  if ((event->type == MOUSEMOVE || event->type == EVT_MODAL_MAP) &&
      (ret & OPERATOR_RUNNING_MODAL))
  {
    image_select_circle_status_update(C, op);
  }
  else if ((ret & OPERATOR_RUNNING_MODAL) == 0) {
    image_select_status_clear(C);
  }
  return ret;
}

void PAINT_OT_image_select_circle(wmOperatorType *ot)
{
  ot->name = "Select Circle";
  ot->idname = "PAINT_OT_image_select_circle";
  ot->description = "Select a circular region as a paint mask";

  ot->invoke = image_select_circle_invoke;
  ot->modal = image_select_circle_modal;
  ot->exec = image_select_circle_exec;
  ot->cancel = WM_gesture_circle_cancel;
  ot->poll = image_paint_selection_poll;
  ot->flag = IMAGE_SELECT_GESTURE_OPTYPE_FLAGS;

  WM_operator_properties_gesture_circle(ot);
  WM_operator_properties_select_operation_simple(ot);

  image_select_gesture_properties(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Circle Select Radius (F)
 * \{ */

/** Drag state for #PAINT_OT_image_select_circle_radius. */
struct ImageSelectCircleRadiusData {
  int start_radius;
  int start_x;
};

/** Write the radius into the active tool's stored operator properties. */
static bool image_select_circle_radius_store(bContext *C, const int radius)
{
  bToolRef *tref = WM_toolsystem_ref_from_context(C);
  wmOperatorType *ot_circle = WM_operatortype_find("paint.image_select_circle", false);
  if (!tref || !STREQ(tref->idname, "builtin.select_circle") || !ot_circle) {
    return false;
  }
  PointerRNA props;
  WM_toolsystem_ref_properties_ensure_from_operator(tref, ot_circle, &props);
  RNA_int_set(&props, "radius", radius);
  return true;
}

static wmOperatorStatus image_select_circle_radius_invoke(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->image) {
    return OPERATOR_CANCELLED;
  }
  bToolRef *tref = WM_toolsystem_ref_from_context(C);
  wmOperatorType *ot_circle = WM_operatortype_find("paint.image_select_circle", false);
  if (!tref || !STREQ(tref->idname, "builtin.select_circle") || !ot_circle) {
    return OPERATOR_CANCELLED;
  }
  PointerRNA props;
  WM_toolsystem_ref_properties_ensure_from_operator(tref, ot_circle, &props);
  const int start_radius = RNA_int_get(&props, "radius");
  if (start_radius <= 0) {
    return OPERATOR_CANCELLED;
  }

  auto *data = MEM_new<ImageSelectCircleRadiusData>(__func__);
  data->start_radius = start_radius;
  data->start_x = event->xy[0];
  op->customdata = data;

  WM_event_add_modal_handler(C, op);
  ED_area_status_text(CTX_wm_area(C),
                      "Move to set the circle radius, LMB or Enter confirms, Esc cancels");
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus image_select_circle_radius_modal(bContext *C,
                                                         wmOperator *op,
                                                         const wmEvent *event)
{
  ImageSelectCircleRadiusData *data = static_cast<ImageSelectCircleRadiusData *>(op->customdata);
  if (!data) {
    return OPERATOR_CANCELLED;
  }

  switch (event->type) {
    case MOUSEMOVE: {
      const int radius = std::max(1, data->start_radius + (event->xy[0] - data->start_x));
      if (image_select_circle_radius_store(C, radius)) {
        char text[64];
        SNPRINTF(text, "Radius: %d", radius);
        ED_area_status_text(CTX_wm_area(C), text);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case LEFTMOUSE: {
      if (event->val == KM_PRESS) {
        image_select_status_clear(C);
        MEM_delete(data);
        op->customdata = nullptr;
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case EVT_RETKEY:
    case EVT_PADENTER: {
      if (event->val == KM_PRESS) {
        image_select_status_clear(C);
        MEM_delete(data);
        op->customdata = nullptr;
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case EVT_ESCKEY:
    case RIGHTMOUSE: {
      if (event->val == KM_PRESS) {
        image_select_circle_radius_store(C, data->start_radius);
        image_select_status_clear(C);
        MEM_delete(data);
        op->customdata = nullptr;
        ED_region_tag_redraw(CTX_wm_region(C));
        return OPERATOR_CANCELLED;
      }
      return OPERATOR_RUNNING_MODAL;
    }
    default:
      return OPERATOR_RUNNING_MODAL;
  }
}

static void image_select_circle_radius_cancel(bContext *C, wmOperator *op)
{
  ImageSelectCircleRadiusData *data = static_cast<ImageSelectCircleRadiusData *>(op->customdata);
  if (!data) {
    return;
  }
  image_select_circle_radius_store(C, data->start_radius);
  image_select_status_clear(C);
  MEM_delete(data);
  op->customdata = nullptr;
}

void PAINT_OT_image_select_circle_radius(wmOperatorType *ot)
{
  ot->name = "Adjust Circle Select Radius";
  ot->idname = "PAINT_OT_image_select_circle_radius";
  ot->description =
      "Interactively change the radius of the Circle Select tool (same as the F radial control)";

  ot->invoke = image_select_circle_radius_invoke;
  ot->modal = image_select_circle_radius_modal;
  ot->cancel = image_select_circle_radius_cancel;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session lifetime
 * \{ */

void paint_select_session_free(PaintSelectSession &session)
{
  image_select_floating_session_free(session.active);
  session.active = nullptr;
}

void ED_image_paint_select_session_free(SpaceImage *sima)
{
  if (!sima || !sima->runtime) {
    return;
  }
  /* Release the canvas-borrow token before the slot itself is freed below. This deliberately
   * does not go through #image_select_session_clear, which would also null `paint_select.active`
   * -- #paint_select_session_free still needs that pointer to free the concrete session through
   * its tool's own destructor. Skipping the borrow release here would leave
   * #bke::ImageRuntime::paint_selection_borrowed_by pointing at a session that no longer exists,
   * permanently locking the canvas out of every Image Editor showing this Image. */
  if (sima->image && sima->image->runtime &&
      sima->image->runtime->paint_selection_borrowed_by == sima)
  {
    sima->image->runtime->paint_selection_borrowed_by = nullptr;
  }
  paint_select_session_free(sima->runtime->paint_select);
}

void ED_image_paint_select_session_cancel(bContext *C, SpaceImage *sima)
{
  if (!sima || !sima->runtime) {
    return;
  }
  image_select_floating_sessions_cancel(C, sima);
}

void ED_image_paint_select_transform_state_free(SpaceImage *sima)
{
  ImageSelectTransformState *state = image_select_transform_state_get(sima);
  if (!state) {
    return;
  }
  image_select_session_clear(sima);
  image_select_transform_state_free(state);
}

/** \} */

} /* namespace blender */
