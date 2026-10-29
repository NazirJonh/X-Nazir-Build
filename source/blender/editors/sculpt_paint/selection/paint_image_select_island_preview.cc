/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Preview of the UV islands a box gesture reaches while an island-expansion drag runs: they are
 * drawn before the release commits anything, each island in its own color so neighboring picks
 * are told apart -- the translucent scheme of the Mask UV Island picker (red while the gesture
 * subtracts). The state dies with the gesture, through #wmGesture::user_data.
 *
 * Seeds and flood follow the commit path (#image_paint_selection_expand_for_object: UV bounds
 * intersect the rect, no delimit), so the preview shows exactly what the release selects. The
 * canvas objects and their BMesh data are cached for the gesture duration, and the islands are
 * re-collected only when the gesture rect changes.
 */

#include <algorithm>
#include <cmath>

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_math_color.h"
#include "BLI_math_vector_types.hh"
#include "BLI_polyfill_2d.h"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "MEM_guardedalloc.h"

#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_mesh.hh"
#include "BKE_screen.hh"

#include "bmesh.hh"

#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_space_api.hh"
#include "ED_uvedit.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"

#include "RNA_access.hh"

#include "UI_view2d.hh"

#include "WM_types.hh"

#include "paint_image_select_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Preview state
 * \{ */

/**
 * BMesh the preview works on, with explicit ownership: a converting handle owns the BMesh it
 * created from the original Mesh and frees it, a borrowing handle wraps the Edit Mode BMesh
 * (#BKE_editmesh_from_object) that lives elsewhere. The destructor frees exactly what the handle
 * created, replacing the commit path's manual `owns_bm` rule (#image_paint_selection_expand_for_object)
 * in a form cache cleanup cannot get wrong.
 */
class IslandPreviewBMesh {
 public:
  IslandPreviewBMesh() = default;
  IslandPreviewBMesh(BMesh *bm, const bool owned) : bm_(bm), owned_(owned) {}

  IslandPreviewBMesh(IslandPreviewBMesh &&other) : bm_(other.bm_), owned_(other.owned_)
  {
    other.bm_ = nullptr;
    other.owned_ = false;
  }
  IslandPreviewBMesh &operator=(IslandPreviewBMesh &&other)
  {
    if (this != &other) {
      this->reset();
      this->bm_ = other.bm_;
      this->owned_ = other.owned_;
      other.bm_ = nullptr;
      other.owned_ = false;
    }
    return *this;
  }

  ~IslandPreviewBMesh()
  {
    this->reset();
  }

  BMesh *get() const
  {
    return this->bm_;
  }

  void reset()
  {
    if (this->owned_ && this->bm_ != nullptr) {
      BM_mesh_free(this->bm_);
    }
    this->bm_ = nullptr;
    this->owned_ = false;
  }

 private:
  BMesh *bm_ = nullptr;
  /** True when #bm_ was created here and must be freed, false for a borrowed Edit Mode BMesh. */
  bool owned_ = false;
};

/** Cached BMesh and UV data for one canvas object during the gesture. */
struct ImageSelectIslandPreviewObjectCache {
  Object *object = nullptr;
  /** Empty for an Edit Mode object: its `em->bm` is fetched per refresh instead, so an undo
   * during the drag cannot leave a dangling pointer. */
  IslandPreviewBMesh bm;
  BMUVOffsets offsets{};
};

/**
 * One object's islands under the box, in flat arrays: every island's face polygons are
 * consecutive vertices, delimited by #ImageSelectIslandPreviewObject::polygon_ends, and the
 * islands themselves are consecutive polygons, delimited by #island_ends. The draw callback walks
 * them without per-polygon allocations.
 */
struct ImageSelectIslandPreviewObject {
  /** UV vertices of every island's face polygons, consecutive. */
  Vector<float2> poly_vertices;
  /** End vertex index of every polygon, ascending. */
  Vector<int> polygon_ends;
  /** End polygon index of every island, ascending. */
  Vector<int> island_ends;
};

struct ImageSelectIslandPreview {
  Vector<ImageSelectIslandPreviewObject> objects;
  /** Cached canvas objects and their BMesh data for the gesture duration. */
  Vector<ImageSelectIslandPreviewObjectCache> cached_objects;
  /** True once the canvas object list was resolved, even when it is empty. */
  bool cache_built = false;
  /** Scene cached for the gesture. */
  Scene *scene = nullptr;
  /** True while the gesture subtracts (its `mode` property): every island reads red, like the
   * island picker. */
  bool subtract = false;
  ARegion *gesture_region = nullptr;
  ARegionType *region_type = nullptr;
  void *draw_handle = nullptr;
  /** The rect the islands were collected for, to skip re-floods while the box holds still. */
  rctf last_uv_rect{};
  bool has_last_rect = false;

  ~ImageSelectIslandPreview()
  {
    if (this->draw_handle != nullptr && this->region_type != nullptr) {
      ED_region_draw_cb_exit(this->region_type, this->draw_handle);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

/** Stack buffer size covering the common triangle/quad face in the draw callback. */
constexpr int IMAGE_SELECT_ISLAND_PREVIEW_STACK_POLY_LEN = 8;

/**
 * Map one face polygon from UV to region space.
 * \return The polygon view into \a r_stack_poly (valid while \a polygon lives).
 */
static Span<float2> image_select_island_preview_polygon_region(
    const ARegion &region,
    const Span<float2> polygon,
    Vector<float2, IMAGE_SELECT_ISLAND_PREVIEW_STACK_POLY_LEN> &r_stack_poly)
{
  r_stack_poly.reinitialize(polygon.size());
  for (const int i : polygon.index_range()) {
    ui::view2d_view_to_region_fl(
        &region.v2d, polygon[i].x, polygon[i].y, &r_stack_poly[i].x, &r_stack_poly[i].y);
  }
  return r_stack_poly;
}

/**
 * Fill and outline one island in one color: the fill triangulates every face polygon
 * (#BLI_polyfill_calc, so n-gons fill too), the outline draws every polygon's edges as one
 * `GPU_PRIM_LINES` batch. Both are a single `immBegin` per island; the per-polygon buffers are
 * stack-sized for the common triangle/quad, so the draw callback allocates nothing.
 */
static void image_select_island_preview_island_draw(const ARegion &region,
                                                    const ImageSelectIslandPreviewObject &entry,
                                                    const int island_poly_begin,
                                                    const int island_poly_end,
                                                    const float4 &fill,
                                                    const float4 &line,
                                                    const uint pos)
{
  int fill_vert_count = 0;
  int line_vert_count = 0;
  for (const int p : IndexRange(island_poly_begin, island_poly_end - island_poly_begin)) {
    const int vert_begin = (p == 0) ? 0 : entry.polygon_ends[p - 1];
    const int poly_len = entry.polygon_ends[p] - vert_begin;
    fill_vert_count += (poly_len - 2) * 3;
    line_vert_count += poly_len * 2;
  }

  Vector<float2, IMAGE_SELECT_ISLAND_PREVIEW_STACK_POLY_LEN> stack_poly;
  Vector<uint3, IMAGE_SELECT_ISLAND_PREVIEW_STACK_POLY_LEN> stack_tris;

  immUniformColor4fv(fill);
  immBegin(GPU_PRIM_TRIS, uint(fill_vert_count));
  for (const int p : IndexRange(island_poly_begin, island_poly_end - island_poly_begin)) {
    const int vert_begin = (p == 0) ? 0 : entry.polygon_ends[p - 1];
    const Span<float2> region_poly = image_select_island_preview_polygon_region(
        region,
        Span<float2>(entry.poly_vertices.data() + vert_begin, entry.polygon_ends[p] - vert_begin),
        stack_poly);
    if (region_poly.size() < 3) {
      continue;
    }
    /* Triangulate in region space, so the fill follows the view mapping. */
    stack_tris.reinitialize(region_poly.size() - 2);
    BLI_polyfill_calc(reinterpret_cast<const float(*)[2]>(region_poly.data()),
                      uint(region_poly.size()),
                      0,
                      reinterpret_cast<uint(*)[3]>(stack_tris.data()));
    for (const uint3 &tri : stack_tris) {
      for (const int i : IndexRange(3)) {
        const float2 &co = region_poly[tri[i]];
        immVertex2f(pos, co.x, co.y);
      }
    }
  }
  immEnd();

  immUniformColor4fv(line);
  immBegin(GPU_PRIM_LINES, uint(line_vert_count));
  for (const int p : IndexRange(island_poly_begin, island_poly_end - island_poly_begin)) {
    const int vert_begin = (p == 0) ? 0 : entry.polygon_ends[p - 1];
    const Span<float2> region_poly = image_select_island_preview_polygon_region(
        region,
        Span<float2>(entry.poly_vertices.data() + vert_begin, entry.polygon_ends[p] - vert_begin),
        stack_poly);
    for (const int i : region_poly.index_range()) {
      const float2 &co_a = region_poly[i];
      const float2 &co_b = region_poly[(i + 1) % region_poly.size()];
      immVertex2f(pos, co_a.x, co_a.y);
      immVertex2f(pos, co_b.x, co_b.y);
    }
  }
  immEnd();
}

static void image_select_island_preview_draw(const bContext * /*C*/,
                                             ARegion *region,
                                             void *arg)
{
  const ImageSelectIslandPreview &preview = *static_cast<const ImageSelectIslandPreview *>(arg);
  /* The callback list is shared by every region of the type; only the region the box is dragged in
   * has polygons mapped to its view. */
  if (region != preview.gesture_region) {
    return;
  }

  GPU_blend(GPU_BLEND_ALPHA);
  const uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  int global_island_i = 0;
  for (const ImageSelectIslandPreviewObject &entry : preview.objects) {
    int island_poly_begin = 0;
    for (const int island_poly_end : entry.island_ends) {
      /* A distinct hue per island, spread by the golden ratio so neighbors stay apart.
       * Continue hue across all objects so each island gets a unique color. */
      float island_rgb[3];
      hsv_to_rgb(std::fmod(global_island_i * 0.618034f, 1.0f),
                 0.75f,
                 1.0f,
                 &island_rgb[0],
                 &island_rgb[1],
                 &island_rgb[2]);
      const float4 fill = preview.subtract ?
                              float4(1.0f, 0.25f, 0.2f, 0.25f) :
                              float4(island_rgb[0], island_rgb[1], island_rgb[2], 0.22f);
      const float4 line = preview.subtract ?
                              float4(1.0f, 0.25f, 0.2f, 0.95f) :
                              float4(island_rgb[0], island_rgb[1], island_rgb[2], 0.95f);
      image_select_island_preview_island_draw(
          *region, entry, island_poly_begin, island_poly_end, fill, line, pos);
      island_poly_begin = island_poly_end;
      global_island_i++;
    }
  }

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Collection
 * \{ */

/**
 * The islands of every canvas object under \a uv_rect, grouped so each can draw in its own color.
 * Seeds and flood follow the commit path (#image_paint_selection_expand_for_object: UV bounds
 * intersect the rect, no delimit), so the preview shows exactly what the release selects. The
 * expanded faces are then grouped into islands over edge-shared UVs
 * (#BM_loop_uv_share_edge_check), the same UV-connectivity rule the flood itself applies.
 */
static void image_select_island_preview_refresh(bContext *C,
                                                ImageSelectIslandPreview &preview,
                                                const rctf &uv_rect,
                                                const bool subtract)
{
  /* Only recalculate if the rect or the mode changed. */
  if (preview.has_last_rect && BLI_rctf_compare(&preview.last_uv_rect, &uv_rect, 0.0f) &&
      preview.subtract == subtract)
  {
    return;
  }
  Scene *scene = CTX_data_scene(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  preview.last_uv_rect = uv_rect;
  preview.has_last_rect = true;
  preview.subtract = subtract;
  preview.objects.clear();
  if (scene == nullptr || sima == nullptr || sima->image == nullptr) {
    return;
  }

  /* Cache BMesh and UV offsets on first call. */
  if (!preview.cache_built) {
    preview.cache_built = true;
    preview.scene = scene;
    for (Object *ob : image_paint_selection_canvas_objects_get(
             C, sima->image, ImagePaintCanvasPurpose::Mask))
    {
      /* Deliberately the *original* mesh outside Edit Mode, the same BMesh the commit path
       * expands on: the evaluated topology (subdivision, mirror, array) need not match the UV
       * layout the user authored and sees in the editor. */
      IslandPreviewBMesh bm;
      if (ob->mode & OB_MODE_EDIT) {
        const BMEditMesh *em = BKE_editmesh_from_object(ob);
        if (em != nullptr && em->bm != nullptr) {
          bm = IslandPreviewBMesh(em->bm, false);
        }
      }
      else {
        const Mesh *mesh = id_cast<const Mesh *>(ob->data);
        if (mesh == nullptr) {
          continue;
        }
        const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);
        BMeshCreateParams create_params{};
        BMeshFromMeshParams convert_params{};
        /* Normals are not used by the island collection. */
        convert_params.calc_face_normal = false;
        convert_params.calc_vert_normal = false;
        BMesh *converted = BM_mesh_create(&allocsize, &create_params);
        BM_mesh_bm_from_me(converted, mesh, &convert_params);
        if (converted->totface != mesh->faces_num) {
          /* Degenerate faces would shift the face indices. */
          BM_mesh_free(converted);
          continue;
        }
        bm = IslandPreviewBMesh(converted, true);
      }

      if (bm.get() == nullptr) {
        continue;
      }

      const BMUVOffsets offsets = image_paint_selection_uv_offsets_get(bm.get(), ob, scene);
      if (offsets.uv < 0) {
        continue;
      }

      ImageSelectIslandPreviewObjectCache cache;
      cache.object = ob;
      /* An Edit Mode BMesh is only borrowed for the offsets lookup above: dropping it here makes
       * the refresh fetch `em->bm` again, so an undo during the drag cannot leave it dangling. */
      cache.bm = std::move(bm);
      if (ob->mode & OB_MODE_EDIT) {
        cache.bm.reset();
      }
      cache.offsets = offsets;
      preview.cached_objects.append(std::move(cache));
    }
  }

  /* Use cached BMesh data to find islands. */
  for (const ImageSelectIslandPreviewObjectCache &cache : preview.cached_objects) {
    BMesh *bm = cache.bm.get();
    if (bm == nullptr) {
      const BMEditMesh *em = BKE_editmesh_from_object(cache.object);
      bm = (em != nullptr) ? em->bm : nullptr;
    }
    if (bm == nullptr) {
      continue;
    }
    const BMUVOffsets &offsets = cache.offsets;

    Vector<int> seed_faces;
    seed_faces.reserve(bm->totface);
    BMIter fiter;
    BMFace *efa;
    int face_index;
    BM_ITER_MESH_INDEX (efa, &fiter, bm, BM_FACES_OF_MESH, face_index) {
      if (BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
        continue;
      }
      if (image_select_face_uv_intersects_rect(*efa, offsets, uv_rect)) {
        seed_faces.append(face_index);
      }
    }

    if (seed_faces.is_empty()) {
      continue;
    }

    /* Single multi-seed flood to find all connected faces at once. */
    Array<bool> expanded(bm->totface, false);
    ED_uvedit_uv_islands_tag_from_face_indices(
        preview.scene, bm, offsets, seed_faces, 0, expanded);

    /* Group the expanded faces into islands: a face walk over the expanded set, where two faces
     * belong to the same island when they share an edge whose UVs agree on both sides
     * (#BM_loop_uv_share_edge_check), exactly the rule the flood itself uses. */
    ImageSelectIslandPreviewObject entry;
    Array<bool> grouped(bm->totface, false);
    BM_mesh_elem_table_ensure(bm, BM_FACE);
    BM_mesh_elem_index_ensure(bm, BM_FACE);

    for (const int start : expanded.index_range()) {
      if (!expanded[start] || grouped[start]) {
        continue;
      }

      /* Mark this connected component. */
      Vector<int> stack;
      stack.append(start);
      const int island_poly_begin = entry.polygon_ends.size();
      const int island_vert_begin = entry.poly_vertices.size();

      while (!stack.is_empty()) {
        const int face_i = stack.pop_last();
        if (grouped[face_i]) {
          continue;
        }
        grouped[face_i] = true;

        BMFace *face = BM_face_at_index(bm, face_i);
        if (face == nullptr || BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
          continue;
        }

        /* Collect face UV polygon. */
        const int poly_vert_begin = entry.poly_vertices.size();
        BMIter liter;
        BMLoop *l;
        BM_ITER_ELEM (l, &liter, face, BM_LOOPS_OF_FACE) {
          const float *uv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
          entry.poly_vertices.append(float2(uv[0], uv[1]));
        }
        if (entry.poly_vertices.size() - poly_vert_begin < 3) {
          /* Fewer than three UVs cannot form a polygon; drop the vertices again. */
          entry.poly_vertices.resize(poly_vert_begin);
          continue;
        }
        entry.polygon_ends.append(entry.poly_vertices.size());

        /* Push UV-connected neighbors onto the stack. */
        BMLoop *l_first, *l_iter;
        l_iter = l_first = BM_FACE_FIRST_LOOP(face);
        do {
          BMLoop *l_radial = l_iter->radial_next;
          while (l_radial != l_iter) {
            if (l_radial->f == face) {
              break;
            }
            const int neighbor_i = BM_elem_index_get(l_radial->f);
            if (expanded[neighbor_i] && !grouped[neighbor_i] &&
                BM_loop_uv_share_edge_check(l_iter, l_radial, offsets.uv))
            {
              stack.append(neighbor_i);
            }
            l_radial = l_radial->radial_next;
          }
        } while ((l_iter = l_iter->next) != l_first);
      }

      if (entry.polygon_ends.size() > island_poly_begin) {
        entry.island_ends.append(entry.polygon_ends.size());
      }
      else {
        /* An island without polygons (every face degenerate): drop the vertices it collected. */
        entry.poly_vertices.resize(island_vert_begin);
      }
    }

    if (!entry.island_ends.is_empty()) {
      preview.objects.append(std::move(entry));
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator hooks
 * \{ */

void image_select_box_island_preview_invoke(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (scene == nullptr || sima == nullptr || sima->image == nullptr) {
    return;
  }
  if (scene->toolsettings->imapaint.selection_expand != IMAGE_PAINT_SELECT_EXPAND_ISLAND) {
    return;
  }
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  ARegion *region = CTX_wm_region(C);
  if (gesture == nullptr || region == nullptr || region->runtime->type == nullptr) {
    return;
  }

  ImageSelectIslandPreview *preview = MEM_new<ImageSelectIslandPreview>(__func__);
  preview->gesture_region = region;
  preview->region_type = region->runtime->type;
  preview->draw_handle = ED_region_draw_cb_activate(
      preview->region_type, image_select_island_preview_draw, preview, REGION_DRAW_POST_PIXEL);
  gesture->user_data.data = preview;
  gesture->user_data.free_fn = [](void *data) {
    MEM_delete(static_cast<ImageSelectIslandPreview *>(data));
  };
  gesture->user_data.use_free = true;
}

/** Refresh the preview from the gesture's current rect; called on every move of the drag. */
void image_select_box_island_preview_update(bContext *C, wmOperator *op)
{
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  if (gesture == nullptr || gesture->user_data.data == nullptr) {
    return;
  }
  ImageSelectIslandPreview *preview = static_cast<ImageSelectIslandPreview *>(
      gesture->user_data.data);
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr || region != preview->gesture_region) {
    return;
  }
  float2 uv_points[4];
  rctf uv_rect;
  if (!image_select_box_uv_bounds_calc(*region, *op, MutableSpan<float2>(uv_points, 4), uv_rect)) {
    return;
  }
  /* The subtract mode is the operator's own `mode` property, the same source the exec reads, so
   * the preview and the release can never disagree. */
  const bool subtract = eSelectOp(RNA_enum_get(op->ptr, "mode")) == SEL_OP_SUB;
  image_select_island_preview_refresh(C, *preview, uv_rect, subtract);
  ED_region_tag_redraw(region);
}

/** \} */

}  // namespace blender
