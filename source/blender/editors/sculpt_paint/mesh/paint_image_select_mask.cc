/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_bitmap_draw_2d.h"
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
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_blender.hh"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_editmesh.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_layer.hh"
#include "BKE_library.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
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
#include "WM_types.hh"

#include "../../space_image/image_runtime.hh"
#include "../paint_intern.hh"
#include "paint_image_select_gesture.hh"
#include "paint_image_select_gradient.hh"
#include "paint_image_select_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Internal helpers
 * \{ */

/**
 * Poll for all image paint selection operators.
 * Does not require an active brush -- selection tools are independent of the brush.
 * Blocked while a free-transform is in progress so that stray LMB clicks outside the
 * transform widget cannot accidentally start a new selection gesture.
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
  if (image_select_transform_is_floating(C)) {
    return false;
  }
  if (image_select_gradient_is_floating(C)) {
    return false;
  }
  if (image_select_warp_is_floating(C)) {
    return false;
  }
  const ARegion *region = CTX_wm_region(C);
  if (!region || region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }
  return true;
}

/**
 * Visit every pixel center covered by a triangle given in tile-local pixel space.
 * \a fn is invoked as `fn(x, y)` and returns `true` to continue or `false` to stop early.
 * Returns `false` if \a fn requested an early stop, otherwise `true`.
 */
template<typename Fn>
static bool foreach_triangle_pixel(const float2 &p0,
                                   const float2 &p1,
                                   const float2 &p2,
                                   const int width,
                                   const int height,
                                   Fn &&fn)
{
  int min_x = int(floorf(std::min({p0.x, p1.x, p2.x})));
  int max_x = int(ceilf(std::max({p0.x, p1.x, p2.x})));
  int min_y = int(floorf(std::min({p0.y, p1.y, p2.y})));
  int max_y = int(ceilf(std::max({p0.y, p1.y, p2.y})));

  min_x = max_ii(min_x, 0);
  min_y = max_ii(min_y, 0);
  max_x = min_ii(max_x, width - 1);
  max_y = min_ii(max_y, height - 1);

  const auto edge_fn = [](const float2 &a, const float2 &b, const float fx, const float fy) {
    return (fx - a.x) * (b.y - a.y) - (fy - a.y) * (b.x - a.x);
  };

  for (int y = min_y; y <= max_y; y++) {
    for (int x = min_x; x <= max_x; x++) {
      const float fx = float(x) + 0.5f;
      const float fy = float(y) + 0.5f;
      const float w0 = edge_fn(p1, p2, fx, fy);
      const float w1 = edge_fn(p2, p0, fx, fy);
      const float w2 = edge_fn(p0, p1, fx, fy);
      const bool has_neg = (w0 < 0.0f) || (w1 < 0.0f) || (w2 < 0.0f);
      const bool has_pos = (w0 > 0.0f) || (w1 > 0.0f) || (w2 > 0.0f);
      if (has_neg == has_pos) {
        continue;
      }
      if (!fn(x, y)) {
        return false;
      }
    }
  }
  return true;
}

/**
 * Tessellate a face's UV polygon into tile-local pixel space and visit every covered pixel.
 * Coordinates are mapped to pixels via `(uv - uv_origin) * size`; the point-in-triangle test is
 * orientation-preserving under this map, so the same primitive serves both reads and writes.
 * See #foreach_triangle_pixel for the \a fn contract; an early stop propagates across triangles.
 *
 * The polygon is triangulated with #BLI_polyfill_calc rather than fanned from vertex 0: a face
 * that is convex in 3D can still be concave in UV space, and a fan over a concave polygon emits
 * triangles that cover area outside the face, flooding the mask beyond the UV island.
 */
template<typename Fn>
static void foreach_face_pixel(const BMFace *efa,
                               const BMUVOffsets &offsets,
                               const float2 &uv_origin,
                               const int width,
                               const int height,
                               Fn &&fn)
{
  Vector<float2, 8> px_verts;
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, const_cast<BMFace *>(efa), BM_LOOPS_OF_FACE) {
    const float *uv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
    px_verts.append(float2((uv[0] - uv_origin.x) * width, (uv[1] - uv_origin.y) * height));
  }

  const int verts_num = px_verts.size();
  if (verts_num < 3) {
    return;
  }

  if (verts_num == 3) {
    foreach_triangle_pixel(px_verts[0], px_verts[1], px_verts[2], width, height, fn);
    return;
  }

  Vector<uint3, 8> tris(verts_num - 2);
  BLI_polyfill_calc(reinterpret_cast<const float(*)[2]>(px_verts.data()),
                    uint(verts_num),
                    0,
                    reinterpret_cast<uint(*)[3]>(tris.data()));

  for (const uint3 &tri : tris) {
    if (!foreach_triangle_pixel(
            px_verts[tri[0]], px_verts[tri[1]], px_verts[tri[2]], width, height, fn))
    {
      return;
    }
  }
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
  foreach_face_pixel(efa, offsets, uv_origin, width, mask->y, [&](const int x, const int y) {
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

  foreach_face_pixel(efa, offsets, uv_origin, width, mask->y, [&](const int x, const int y) {
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

static Vector<Object *> image_paint_selection_canvas_objects_get(const bContext *C,
                                                                 const Image *image)
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

  for (Object *ob : BKE_view_layer_array_from_objects_in_edit_mode_unique_data_with_uvs(
           *bmain, scene, view_layer, nullptr))
  {
    add_if_uses_image(ob);
  }

  /* The Image Editor paints sima->image directly, so the active object is a legitimate canvas even
   * when no material references the image; #mesh_object_uses_image covers that case. There is
   * deliberately no "no object matched, so use every mesh in the view layer" fallback: rasterizing
   * the UV islands of arbitrary scene objects into the mask bleeds the selection across unrelated
   * UV layouts, which is both surprising and hard to diagnose. */
  add_if_uses_image(CTX_data_active_object(C));

  return objects;
}

static BMUVOffsets image_paint_selection_uv_offsets_get(BMesh *bm, Object *ob, const Scene *scene)
{
  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;

  if (imapaint.mode == IMAGEPAINT_MODE_MATERIAL) {
    Material *ma = BKE_object_material_get(ob, ob->actcol);
    if (ma && ma->texpaintslot && ma->paint_active_slot < ma->tot_slots) {
      const char *uvname = ma->texpaintslot[ma->paint_active_slot].uvname;
      if (uvname && uvname[0]) {
        const int layer = CustomData_get_named_layer_index(&bm->ldata, CD_PROP_FLOAT2, uvname);
        if (layer != -1) {
          return BM_uv_map_offsets_from_layer(bm, layer);
        }
      }
    }
  }

  if (const std::optional<StringRef> uv_name = BKE_paint_canvas_uvmap_name_get(
          &scene->toolsettings->paint_mode, ob))
  {
    const int layer = CustomData_get_named_layer_index(
        &bm->ldata, CD_PROP_FLOAT2, uv_name->data());
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
 */
static void image_paint_selection_expand_uv_islands_for_object(
    Scene *scene,
    Object *ob,
    Image *image,
    const float fill_value,
    const float threshold,
    const rctf *gesture_uv_bounds)
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

  /* ED_uvedit_uv_islands_tag_from_face_indices calls uvedit_face_visible_test which
   * requires BM_ELEM_SELECT when UV sync is off.  A BMesh freshly converted from an
   * Object-mode mesh has no face selection state, so every seed would be rejected.
   * Mark all non-hidden faces as selected so the flood-fill can reach them. */
  if (owns_bm) {
    BMIter fiter_sel;
    BMFace *efa_sel;
    BM_ITER_MESH (efa_sel, &fiter_sel, bm, BM_FACES_OF_MESH) {
      if (!BM_elem_flag_test(efa_sel, BM_ELEM_HIDDEN)) {
        BM_elem_flag_enable(efa_sel, BM_ELEM_SELECT);
      }
    }
  }

  Array<bool> island_tags(bm->totface);
  ED_uvedit_uv_islands_tag_from_face_indices(
      scene, bm, offsets, seed_faces, 0, island_tags);

  /* Bucket island faces by the UDIM tile(s) their UVs fall on, in a single pass over the mesh.
   * This avoids re-scanning every face once per tile during rasterization (was O(tiles * faces)).
   * A face straddling a tile border is registered with every tile it overlaps; the rasterizer
   * clips to each tile's pixel bounds, so per-tile clipping stays correct. */
  Map<int, Vector<int>> tile_faces;
  {
    BMIter fiter;
    BMFace *efa;
    int face_index;
    BM_ITER_MESH_INDEX (efa, &fiter, bm, BM_FACES_OF_MESH, face_index) {
      if (!island_tags[face_index]) {
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

void image_paint_selection_expand_uv_islands(bContext *C,
                                             Image *image,
                                             const eSelectOp sel_op,
                                             const rctf *gesture_uv_bounds)
{
  Scene *scene = CTX_data_scene(C);
  if (!scene->toolsettings->imapaint.use_selection_uv_island) {
    return;
  }
  if (sel_op == SEL_OP_SUB && !BKE_image_paint_selection_mask_has_any(image)) {
    return;
  }

  const float fill_value = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;
  const float threshold = SELECTION_MASK_THRESHOLD;

  Vector<Object *> objects = image_paint_selection_canvas_objects_get(C, image);

  for (Object *ob : objects) {
    image_paint_selection_expand_uv_islands_for_object(
        scene, ob, image, fill_value, threshold, gesture_uv_bounds);
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

  Scene *scene = CTX_data_scene(C);

  /* Only select the active tile, not all tiles. */
  ImageUser iuser = sima->iuser;
  /* For UDIM images, sima->iuser.tile is reset to 0 after each temporary acquire.
   * The true active tile is tracked by ima->active_tile_index. */
  if (image->source == IMA_SRC_TILED) {
    const ImageTile *active = static_cast<const ImageTile *>(
        BLI_findlink(&image->tiles, image->active_tile_index));
    if (active == nullptr) {
      /* A tiled image can legitimately have an out-of-range active index; fall back to the first
       * tile, and bail out entirely when the tile list is empty. */
      active = static_cast<const ImageTile *>(image->tiles.first);
    }
    if (active == nullptr) {
      return OPERATOR_CANCELLED;
    }
    iuser.tile = active->tile_number;
  }
  const ImageTile *active_tile = BKE_image_get_tile_from_iuser(image, &iuser);
  if (!active_tile) {
    return OPERATOR_CANCELLED;
  }
  iuser.tile = active_tile->tile_number;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
  if (!ibuf) {
    return OPERATOR_CANCELLED;
  }

  ED_image_undo_push_begin_selection("Select All", image);

  BKE_image_paint_selection_mask_get(image, active_tile->tile_number, ibuf->x, ibuf->y);
  BKE_image_paint_selection_mask_fill(image, active_tile->tile_number, 1.0f);
  BKE_image_paint_selection_edge_policy_set(image, BKE_image_paint_selection_edge_policy_hard());

  BKE_image_release_ibuf(image, ibuf, nullptr);

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  BKE_image_paint_selection_blend_mask_invalidate(image);
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

  Image *image = sima->image;

  if (image) {
    ED_image_undo_push_begin_selection("Select None", image);
  }

  if (image) {
    BKE_image_paint_selection_mask_free(image);
  }

  Scene *scene = CTX_data_scene(C);

  DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  if (image) {
    ED_image_undo_push_end();
  }
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

  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_scene(C);

  ED_image_undo_push_begin_selection("Invert Selection", image);

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = sima->iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }

    BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);
    BKE_image_paint_selection_mask_invert(image, tile->tile_number);

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

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

/**
 * Rectangular gesture. The only shape whose coverage is exactly the per-tile intersection
 * rectangle, so #rasterize_tile can fill \a tile_uv_rect directly.
 */
class ImageSelectBoxShape : public ImageSelectGestureShape {
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

    ui::view2d_region_to_view_rctf(&region->v2d, &rectf, &rectf);

    /* Auto-snap selection bounds to UDIM tile borders (integer UV coordinates) when close
     * enough. This makes it easy to select exactly one full tile or align to tile edges
     * without pixel-perfect cursor placement. Threshold: 2 % of one tile in UV units. */
    const float snap_thresh = 0.02f;
    auto snap_to_udim_border = [](float v, float threshold) -> float {
      const float rounded = roundf(v);
      return (fabsf(v - rounded) < threshold) ? rounded : v;
    };
    rectf.xmin = snap_to_udim_border(rectf.xmin, snap_thresh);
    rectf.xmax = snap_to_udim_border(rectf.xmax, snap_thresh);
    rectf.ymin = snap_to_udim_border(rectf.ymin, snap_thresh);
    rectf.ymax = snap_to_udim_border(rectf.ymax, snap_thresh);

    r_uv_bounds = rectf;
    return true;
  }

  void rasterize_tile(const float2 &uv_origin,
                      const rctf &tile_uv_rect,
                      ImBuf *mask,
                      const float fill_value) const override
  {
    float *data = mask->float_data_for_write();
    const int x1 = int(roundf((tile_uv_rect.xmin - uv_origin.x) * mask->x));
    const int y1 = int(roundf((tile_uv_rect.ymin - uv_origin.y) * mask->y));
    const int x2 = int(roundf((tile_uv_rect.xmax - uv_origin.x) * mask->x));
    const int y2 = int(roundf((tile_uv_rect.ymax - uv_origin.y) * mask->y));

    for (int y = y1; y < y2; y++) {
      for (int x = x1; x < x2; x++) {
        if (x >= 0 && x < mask->x && y >= 0 && y < mask->y) {
          data[y * mask->x + x] = fill_value;
        }
      }
    }
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

static wmOperatorStatus image_select_box_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent *event)
{
  if (image_select_gesture_invoke_begin(C, op, event)) {
    return OPERATOR_FINISHED;
  }
  return WM_gesture_box_invoke(C, op, event);
}

static wmOperatorStatus image_select_box_modal(bContext *C,
                                               wmOperator *op,
                                               const wmEvent *event)
{
  image_select_gesture_drag_detect(op, event);
  return WM_gesture_box_modal(C, op, event);
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

/** Per-scanline sink for #BLI_bitmap_draw_2d_poly_v2i_n writing into a 1-channel float mask. */
struct MaskPolyFillData {
  float *data;
  int width;
  float fill_value;
};

static void mask_poly_fill_cb(int x, const int x_end, const int y, void *user_data)
{
  const MaskPolyFillData *fill = static_cast<const MaskPolyFillData *>(user_data);
  float *row = fill->data + int64_t(y) * fill->width;
  do {
    row[x] = fill->fill_value;
  } while (++x != x_end);
}

/**
 * Fill the interior of \a points into a 1-channel float buffer.
 *
 * Delegates to #BLI_bitmap_draw_2d_poly_v2i_n, which implements the even-odd rule with tracked
 * sorted spans. That handles self-intersecting lassos (a common freehand accident) correctly and
 * clips spans to the buffer, both of which the previous hand-rolled scanline fill got wrong.
 * Passing `(0, 0, width, height)` as the region makes the callback receive absolute buffer
 * coordinates, since the span callback reports coordinates relative to the region origin.
 */
static void fill_polygon_float(ImBuf *ibuf, const Span<int2> points, const float color)
{
  if (points.size() < 3) {
    return;
  }

  MaskPolyFillData fill{};
  fill.data = ibuf->float_data_for_write();
  fill.width = ibuf->x;
  fill.fill_value = color;

  BLI_bitmap_draw_2d_poly_v2i_n(0, 0, ibuf->x, ibuf->y, points, mask_poly_fill_cb, &fill);
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
    const Array<int2> mcoords = WM_gesture_lasso_path_to_array(C_, op);
    if (mcoords.size() < 3) {
      return false;
    }

    /* Convert lasso points to UV space once. */
    uv_points_.reserve(mcoords.size());
    BLI_rctf_init_minmax(&r_uv_bounds);
    for (const int2 &p : mcoords) {
      float co[2] = {float(p.x), float(p.y)};
      ui::view2d_region_to_view(&region->v2d, co[0], co[1], &co[0], &co[1]);
      uv_points_.append(float2(co[0], co[1]));
      BLI_rctf_do_minmax_v(&r_uv_bounds, co);
    }
    return true;
  }

  void rasterize_tile(const float2 &uv_origin,
                      const rctf & /*tile_uv_rect*/,
                      ImBuf *mask,
                      const float fill_value) const override
  {
    /* Convert UV points to tile pixel space. */
    Vector<int2> tile_points;
    tile_points.reserve(uv_points_.size());
    for (const float2 &uv : uv_points_) {
      tile_points.append(int2(int(roundf((uv.x - uv_origin.x) * mask->x)),
                              int(roundf((uv.y - uv_origin.y) * mask->y))));
    }

    fill_polygon_float(mask, tile_points, fill_value);
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

static wmOperatorStatus image_select_lasso_modal(bContext *C,
                                                 wmOperator *op,
                                                 const wmEvent *event)
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
    const int cx = int(roundf((uv_center_.x - uv_origin.x) * mask->x));
    const int cy = int(roundf((uv_center_.y - uv_origin.y) * mask->y));
    int rx = int(roundf(uv_radius_x_ * mask->x));
    if (rx <= 0) {
      rx = 1;
    }
    /* Compute Y pixel radius from the separately measured UV Y radius so the circle is
     * correct on non-square tiles and in views with a non-1:1 aspect ratio. */
    int ry = int(roundf(uv_radius_y_ * mask->y));
    if (ry <= 0) {
      ry = 1;
    }

    fill_circle_float(mask, cx, cy, rx, ry, fill_value);
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

static wmOperatorStatus image_select_circle_modal(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  image_select_gesture_drag_detect(op, event);
  return WM_gesture_circle_modal(C, op, event);
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
  paint_select_session_free(sima->runtime->paint_select);
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

}  /* namespace blender */
