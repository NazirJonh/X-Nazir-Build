/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spimage
 */

#include "DNA_brush_types.h"
#include "DNA_mask_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_enums.h"

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_uuid.h"

#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_combined.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_scene.hh"

#include "DNA_material_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "ED_image.hh" /* own include */
#include "ED_material_combined.hh"
#include "ED_mesh.hh"
#include "ED_screen.hh"
#include "ED_uvedit.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "image_runtime.hh"

namespace blender {

Image *ED_space_image(const SpaceImage *sima)
{
  /* NOTE: image_panel_properties() uses pointer to `sima->image` directly. */
  return sima->image;
}

void ED_space_image_set(Main *bmain, SpaceImage *sima, Image *ima, bool automatic)
{
  /* Automatically pin image when manually assigned, otherwise it follows object. */
  if (!automatic && sima->image != ima && sima->mode == SI_MODE_UV) {
    sima->pin = true;
  }

  sima->image = ima;

  if (ima == nullptr || ima->type == IMA_TYPE_R_RESULT || ima->type == IMA_TYPE_COMPOSITE) {
    if (sima->mode == SI_MODE_PAINT) {
      sima->mode = SI_MODE_VIEW;
    }
  }

  if (sima->image) {
    BKE_image_signal(bmain, sima->image, &sima->iuser, IMA_SIGNAL_USER_NEW_IMAGE);
  }

  id_us_ensure_real(id_cast<ID *>(sima->image));

  if (ima) {
    sima->xof = ima->runtime->view_offset[0];
    sima->yof = ima->runtime->view_offset[1];
    sima->zoom = ima->runtime->view_zoom;
  }

  WM_main_add_notifier(NC_SPACE | ND_SPACE_IMAGE, nullptr);
}

void ED_space_image_set_ex(Main *bmain, SpaceImage *sima, Image *ima, const bool keep_view)
{
  const float zoom = sima->zoom;
  const float xof = sima->xof;
  const float yof = sima->yof;

  ED_space_image_set(bmain, sima, ima, false);

  if (!keep_view) {
    return;
  }

  sima->zoom = zoom;
  sima->xof = xof;
  sima->yof = yof;
  if (ima != nullptr && ima->runtime != nullptr) {
    ima->runtime->view_zoom = zoom;
    ima->runtime->view_offset[0] = xof;
    ima->runtime->view_offset[1] = yof;
  }
}

void ED_space_image_sync(Main *bmain, Image *image, bool ignore_render_viewer)
{
  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);
  for (wmWindow &win : wm->windows) {
    const bScreen *screen = WM_window_get_active_screen(&win);
    for (ScrArea &area : screen->areabase) {
      for (SpaceLink &sl : area.spacedata) {
        if (sl.spacetype != SPACE_IMAGE) {
          continue;
        }
        SpaceImage *sima = reinterpret_cast<SpaceImage *>(&sl);
        if (sima->pin) {
          continue;
        }
        if (ignore_render_viewer && sima->image &&
            ELEM(sima->image->type, IMA_TYPE_R_RESULT, IMA_TYPE_COMPOSITE))
        {
          continue;
        }
        ED_space_image_set(bmain, sima, image, true);
      }
    }
  }
}

void ED_space_image_paint_auto_select_material_canvas(Main *bmain, Object *ob)
{
  if (bmain == nullptr || ob == nullptr || ob->type != OB_MESH) {
    return;
  }

  Image *image = BKE_paint_material_preferred_display_image(*ob);
  if (image == nullptr) {
    return;
  }

  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);
  if (wm == nullptr) {
    return;
  }
  for (wmWindow &win : wm->windows) {
    const bScreen *screen = WM_window_get_active_screen(&win);
    if (screen == nullptr) {
      continue;
    }
    for (ScrArea &area : screen->areabase) {
      for (SpaceLink &sl : area.spacedata) {
        if (sl.spacetype != SPACE_IMAGE) {
          continue;
        }
        SpaceImage *sima = reinterpret_cast<SpaceImage *>(&sl);
        if (sima->mode != SI_MODE_PAINT || sima->image != nullptr) {
          continue;
        }
        ED_space_image_set(bmain, sima, image, true);
      }
    }
  }
}

void ED_space_image_auto_set(const bContext *C, SpaceImage *sima)
{
  if (sima->mode != SI_MODE_UV || sima->pin) {
    return;
  }

  /* Track image assigned to active face in edit mode. */
  Object *ob = CTX_data_active_object(C);
  if (!(ob && (ob->mode & OB_MODE_EDIT) && ED_space_image_show_uvedit(sima, ob))) {
    return;
  }

  BMEditMesh *em = BKE_editmesh_from_object(ob);
  BMesh *bm = em->bm;
  BMFace *efa = BM_mesh_active_face_get(bm, true, false);
  if (efa == nullptr) {
    return;
  }

  Image *ima = nullptr;
  ED_object_get_active_image(ob, efa->mat_nr + 1, &ima, nullptr, nullptr, nullptr);

  if (ima != sima->image) {
    sima->image = ima;

    if (sima->image) {
      Main *bmain = CTX_data_main(C);
      BKE_image_signal(bmain, sima->image, &sima->iuser, IMA_SIGNAL_USER_NEW_IMAGE);
      WM_main_add_notifier(NC_SPACE | ND_SPACE_IMAGE, sima);
    }
  }
}

Mask *ED_space_image_get_mask(const SpaceImage *sima)
{
  return sima->mask_info.mask;
}

void ED_space_image_set_mask(bContext *C, SpaceImage *sima, Mask *mask)
{
  sima->mask_info.mask = mask;

  /* weak, but same as image/space */
  id_us_ensure_real(id_cast<ID *>(sima->mask_info.mask));

  if (C) {
    WM_event_add_notifier(C, NC_MASK | NA_SELECTED, mask);
  }
}

/**
 * Whether any channel of \a ma resolves to a layer stack that \a image is a layer of.
 *
 * \param layers: scratch space, so that a caller testing many materials allocates once.
 */
static bool space_image_composite_material_contains(
    Main &bmain,
    Material &ma,
    const Image &image,
    Vector<PaintMaterialCompositeImageLayer> &layers)
{
  /* A map that is part of a layer without being wired into the graph -- a baked Ambient Occlusion
   * map, a layer mask -- is recognized through the layer it is tagged with rather than by asking
   * for its own channel. Asking would make this loop derive every channel from the layer tags for
   * every material in the file, which is a graph walk per channel plus a pass over every image,
   * per material, per redraw. */
  const bool image_has_layer_id = !BLI_uuid_is_nil(image.paint_layer_id);

  /* Any channel identifies the material, not just the composited one: the canvas the user came
   * from is as likely to be a Roughness layer as a Base Color one, and switching to the composite
   * should not depend on which channel they were painting. */
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (!BKE_paint_material_composite_stack_from_material(
            bmain, ma, info.channel, layers, /*allow_layer_map_fallback=*/false))
    {
      continue;
    }
    for (const PaintMaterialCompositeImageLayer &layer : layers) {
      if (layer.color_image == &image) {
        return true;
      }
      if (image_has_layer_id && layer.color_image != nullptr &&
          BLI_uuid_equal(layer.color_image->paint_layer_id, image.paint_layer_id))
      {
        return true;
      }
    }
  }
  return false;
}

/**
 * What the last lookup resolved for one image, by #ID.session_uid.
 *
 * A redraw asks for the composite at least once per frame, and the search below is a graph walk
 * per channel per material: without this, a file with many materials pays for all of them every
 * frame to answer a question whose answer almost never changes.
 *
 * A *miss* is remembered as well as a hit, and that is the half that matters for the cost. An
 * image that belongs to no material at all -- an ordinary picture opened while the space happens
 * to be in composite mode -- is the case a hit-only memo never covers, and it is the case that
 * pays the full scan over every material on every single frame.
 *
 * The two halves are trusted differently, because they can go stale differently:
 *
 * - A hit is re-checked against the material it names, which is the very check the full scan
 *   makes, just for one candidate instead of all of them. A wrong hit therefore costs one extra
 *   walk and falls back to the scan.
 * - A miss cannot be re-checked against anything -- confirming it *is* the scan -- so it has to
 *   be given up on instead, whenever something could have turned it into a hit. There are two
 *   such events, and they need different answers:
 *
 *   - An existing material was rewired, which #ED_space_image_composite_material_memo_clear
 *     reports from the same place that already tells the composite cache a node tree changed.
 *   - A material that was not there before arrived already wired -- linked or appended from
 *     another file, or built by a script -- and edits nothing, so nothing reports it. Guarded by
 *     the material count instead, which cannot miss an arrival the way a notifier can. Counting is
 *     a walk over a linked list of pointers; the scan it stands in for is a walk over every
 *     channel of every one of those materials' node graphs.
 *
 * Session UIDs rather than pointers, since a freed material hands its address to the next one.
 *
 * Main thread only, like the composite cache it feeds.
 */
struct CompositeMaterialMemo {
  /** Whether anything is remembered at all. */
  bool is_set = false;
  /** What was asked about. */
  uint32_t image_uid = 0;
  /** Zero when the remembered answer is "no material owns this image". */
  uint32_t material_uid = 0;
  /**
   * How many materials #Main held when this was recorded.
   *
   * Only a remembered miss is held to it. A hit needs no such guard: it is re-checked against the
   * material it names, and a material arriving alongside cannot make that answer wrong -- the scan
   * returns the first material that owns the image, and the remembered one still does.
   */
  int material_num = 0;
};
static CompositeMaterialMemo g_composite_material_memo;

/**
 * The material whose layer stack \a image belongs to, or null.
 *
 * The composite is displayed in place of a canvas image, and the canvas image is what says which
 * material to composite: the user was painting on one of its layers a moment ago. Derived rather
 * than stored so that nothing has to be kept in sync, and so that no #Material pointer has to live
 * in #SpaceImage across undo and file load.
 *
 * Walks #Main because it has nothing better to start from; the memo above is what keeps the
 * steady state from paying for that walk on every redraw.
 */
static Material *space_image_composite_material_find(Main &bmain, const Image &image)
{
  Vector<PaintMaterialCompositeImageLayer> layers;
  const CompositeMaterialMemo &memo = g_composite_material_memo;

  if (memo.is_set && memo.image_uid == image.id.session_uid) {
    if (memo.material_uid == 0) {
      /* A remembered miss, good only for as long as the set of materials it was concluded from is
       * the same one. Confirming it any other way is the scan it exists to avoid. */
      if (memo.material_num == bmain.materials.count()) {
        return nullptr;
      }
    }
    else {
      Material *remembered = id_cast<Material *>(
          BKE_libblock_find_session_uid(&bmain, ID_MA, memo.material_uid));
      if (remembered != nullptr &&
          space_image_composite_material_contains(bmain, *remembered, image, layers))
      {
        return remembered;
      }
    }
  }

  /* Read once for the record below: the scan does not add or remove materials. */
  const int material_num = bmain.materials.count();
  for (Material &ma : bmain.materials) {
    if (!space_image_composite_material_contains(bmain, ma, image, layers)) {
      continue;
    }
    g_composite_material_memo = {true, image.id.session_uid, ma.id.session_uid, material_num};
    return &ma;
  }
  g_composite_material_memo = {true, image.id.session_uid, 0, material_num};
  return nullptr;
}

void ED_space_image_composite_material_memo_clear()
{
  g_composite_material_memo = CompositeMaterialMemo{};
}

Material *ED_space_image_composite_material_get(Main *bmain, const Image *image)
{
  if (bmain == nullptr || image == nullptr) {
    return nullptr;
  }
  return space_image_composite_material_find(*bmain, *image);
}

bool ED_space_image_has_composite(const SpaceImage *sima)
{
  /* Without an image there is no material to find, and no #Image for a release to go through
   * either, which would leak the reference #ED_space_image_acquire_composite_buffer takes. */
  return sima != nullptr && (sima->flag & SI_PAINT_COMPOSITE_MODE) != 0 && sima->image != nullptr;
}

ImBuf *ED_space_image_acquire_composite_buffer(Main *bmain,
                                               SpaceImage *sima,
                                               uint64_t *r_revision,
                                               rcti *r_changed_region)
{
  if (r_revision != nullptr) {
    *r_revision = 0;
  }
  if (r_changed_region != nullptr) {
    BLI_rcti_init(r_changed_region, 0, 0, 0, 0);
  }
  if (!ED_space_image_has_composite(sima) || bmain == nullptr) {
    return nullptr;
  }
  Material *ma = space_image_composite_material_find(*bmain, *sima->image);
  if (ma == nullptr) {
    return nullptr;
  }

  const int pass = sima->material_paint_pass;

  if (pass == PAINT_LAYER_PASS_COMBINED) {
    CombinedPreviewLighting lighting = BKE_paint_material_combined_lighting_default();
    BKE_paint_material_combined_lighting_rotate_z(lighting, sima->material_paint_light_rot_z);
    /* Default-constructed unless the main region armed it, which is the whole of the narrowing
     * contract: a caller that is not drawing -- the eyedropper, the scopes, a save -- gets the
     * whole preview shaded, exactly as before. */
    ed::material_combined::CombinedPreviewRequest request;
    if (sima->runtime != nullptr && sima->runtime->combined_preview_draw.is_armed()) {
      request.clip = sima->runtime->combined_preview_draw.clip;
      request.max_output = sima->runtime->combined_preview_draw.display_size;
    }
    ImBuf *combined = ed::material_combined::combined_preview_ensure(
        *bmain, *ma, lighting, request, r_revision, r_changed_region);
    if (combined == nullptr) {
      /* Nothing resolved: the plain canvas image is shown, exactly as a channel pass that is not a
       * layer stack already behaves. Whatever canvas was remembered belongs to a preview that no
       * longer exists, so it is dropped rather than left to answer #ED_space_image_get_size. */
      if (sima->runtime != nullptr) {
        sima->runtime->combined_preview_canvas_image_uid = 0;
      }
      return nullptr;
    }
    /* Remembered here because this is the once-a-frame point that already holds the material:
     * #ED_space_image_get_size is reached far more often than this and must not repeat the
     * lookup. */
    if (sima->runtime != nullptr) {
      int canvas_width = 0;
      int canvas_height = 0;
      if (BKE_paint_material_combined_cache_size_get(*ma, canvas_width, canvas_height)) {
        sima->runtime->combined_preview_canvas = int2(canvas_width, canvas_height);
        sima->runtime->combined_preview_canvas_image_uid = sima->image->id.session_uid;
      }
      else {
        sima->runtime->combined_preview_canvas_image_uid = 0;
      }
    }
    /* Referenced so #ED_space_image_release_buffer releases it like any other buffer, which is
     * what keeps the two paths symmetrical. */
    IMB_refImBuf(combined);
    return combined;
  }

  Vector<PaintMaterialCompositeImageLayer> layers;
  if (!BKE_paint_material_composite_stack_from_material(*bmain, *ma, pass, layers)) {
    return nullptr;
  }
  const uint64_t stack_hash = BKE_paint_material_composite_stack_hash(layers);
  ImBuf *ibuf = BKE_paint_material_composite_cache_ensure(
      *ma, pass, layers, stack_hash, r_revision);
  if (ibuf == nullptr) {
    return nullptr;
  }
  /* Handed out with a reference of its own so that #ED_space_image_release_buffer can release it
   * exactly like any other buffer. That is what keeps the two paths symmetrical: a caller never
   * has to know which one it got, and a composite that fails here leaves the ordinary path -- and
   * its lock -- untouched. */
  IMB_refImBuf(ibuf);
  return ibuf;
}

ImBuf *ED_space_image_acquire_buffer(SpaceImage *sima,
                                     void **r_lock,
                                     int tile,
                                     const bool ensure_host_buffer)
{
  ImBuf *ibuf;

  if (ED_space_image_has_composite(sima)) {
    /* #G_MAIN because this entry point has no #Main of its own and is reached from too many
     * places to be given one. The per-frame caller -- the image engine -- does not come through
     * here; it calls #ED_space_image_acquire_composite_buffer with its own #Main. */
    if (ImBuf *composite = ED_space_image_acquire_composite_buffer(G_MAIN, sima)) {
      *r_lock = nullptr;
      return composite;
    }
    /* A material that is not a layer stack -- or none at all -- shows its plain canvas image
     * rather than an empty editor. */
  }

  if (sima && sima->image) {
    const Image *image = sima->image;

#if 0
    if (image->type == IMA_TYPE_R_RESULT && BIF_show_render_spare()) {
      return BIF_render_spare_imbuf();
    }
    else
#endif
    {
      sima->iuser.tile = tile;
      if (ensure_host_buffer) {
        ibuf = BKE_image_acquire_ibuf(sima->image, &sima->iuser, r_lock);
      }
      else {
        ibuf = BKE_image_acquire_ibuf_gpu(sima->image, &sima->iuser, r_lock);
      }
      sima->iuser.tile = 0;
    }

    if (ibuf) {
      if (image->type == IMA_TYPE_R_RESULT && ibuf->x != 0 && ibuf->y != 0) {
        /* Render result might be lazily allocated. Return ibuf without buffers to indicate that
         * there is image buffer but it has no data yet. */
        return ibuf;
      }

      if (ibuf->byte_data() || ibuf->float_data() || ibuf->gpu.texture) {
        return ibuf;
      }
      BKE_image_release_ibuf(sima->image, ibuf, *r_lock);
      *r_lock = nullptr;
    }
  }
  else {
    *r_lock = nullptr;
  }

  return nullptr;
}

void ED_space_image_release_buffer(SpaceImage *sima, ImBuf *ibuf, void *lock)
{
  if (sima && sima->image) {
    BKE_image_release_ibuf(sima->image, ibuf, lock);
  }
}

int ED_space_image_get_display_channel_mask(ImBuf *ibuf)
{
  int result = (SI_USE_ALPHA | SI_SHOW_ALPHA | SI_SHOW_ZBUF | SI_SHOW_R | SI_SHOW_G | SI_SHOW_B);
  if (!ibuf) {
    return result;
  }

  const bool color = ibuf->channels >= 3;
  const bool alpha = ibuf->channels == 4;
  const bool zbuf = ibuf->channels == 1;

  if (!alpha) {
    result &= ~(SI_USE_ALPHA | SI_SHOW_ALPHA);
  }
  if (!zbuf) {
    result &= ~SI_SHOW_ZBUF;
  }
  if (!color) {
    result &= ~(SI_SHOW_R | SI_SHOW_G | SI_SHOW_B);
  }
  return result;
}

bool ED_space_image_has_buffer(SpaceImage *sima)
{
  ImBuf *ibuf;
  void *lock;
  bool has_buffer;

  ibuf = ED_space_image_acquire_buffer(sima, &lock, 0, false);
  has_buffer = (ibuf != nullptr);
  ED_space_image_release_buffer(sima, ibuf, lock);

  return has_buffer;
}

void ED_space_image_get_size(SpaceImage *sima, int *r_width, int *r_height)
{
  Scene *scene = sima->iuser.scene;
  ImBuf *ibuf;
  void *lock;

  /* A composite preview answers from its cache rather than by being produced. Acquiring here would
   * shade the whole canvas to read two integers, and this is reached several times per redraw --
   * #image_main_region_set_view2d among them. Falling through when nothing is cached is right:
   * something has to establish the size once, and the acquisition below is that something.
   *
   * Answering from the cache rather than from `sima->image` is deliberate. The gather derives the
   * preview dimensions from the first channel that resolves to a layer stack, which need not equal
   * the canvas image size; the cache preserves whatever it decided.
   *
   * Narrowed to the Combined pass because a channel pass is served by a different cache -- the
   * composite one -- and a stale Combined entry left over from an earlier pass would answer for
   * it. */
  if (ED_space_image_has_composite(sima) && sima->image != nullptr &&
      sima->material_paint_pass == PAINT_LAYER_PASS_COMBINED)
  {
    /* The fast path, and the one nearly every call takes: the answer was recorded by the last
     * #ED_space_image_acquire_composite_buffer, so a redraw's several size queries cost a compare
     * rather than a material lookup each. */
    if (sima->runtime != nullptr &&
        sima->runtime->combined_preview_canvas_image_uid == sima->image->id.session_uid &&
        sima->runtime->combined_preview_canvas.x > 0)
    {
      *r_width = sima->runtime->combined_preview_canvas.x;
      *r_height = sima->runtime->combined_preview_canvas.y;
      return;
    }
    /* Nothing remembered -- a space that has not drawn yet, or one whose image just changed. Ask
     * the cache directly, which still beats producing the preview to measure it. */
    if (Material *ma = ED_space_image_composite_material_get(G_MAIN, sima->image)) {
      if (BKE_paint_material_combined_cache_size_get(*ma, *r_width, *r_height)) {
        return;
      }
    }
    /* Nothing cached yet -- the first frame, or a material that resolves to nothing. The canvas
     * image is the answer, and falling through to the acquisition below is not: that returns the
     * preview buffer, which is shaded at a fraction of the canvas and would report the fraction.
     */
    BKE_image_get_size(sima->image, &sima->iuser, r_width, r_height);
    return;
  }

  /* TODO(lukas): Support tiled images with different sizes */
  ibuf = ED_space_image_acquire_buffer(sima, &lock, 0, false);

  if (ibuf && ibuf->x > 0 && ibuf->y > 0) {
    *r_width = ibuf->x;
    *r_height = ibuf->y;
  }
  else if (sima->image && sima->image->type == IMA_TYPE_R_RESULT && scene) {
    /* not very important, just nice */
    BKE_render_resolution(&scene->r, true, r_width, r_height);
  }
  /* I know a bit weak... but preview uses not actual image size */
  // XXX else if (image_preview_active(sima, r_width, r_height));
  else {
    *r_width = IMG_SIZE_FALLBACK;
    *r_height = IMG_SIZE_FALLBACK;
  }

  ED_space_image_release_buffer(sima, ibuf, lock);
}

void ED_space_image_get_size_fl(SpaceImage *sima, float r_size[2])
{
  int size_i[2];
  ED_space_image_get_size(sima, &size_i[0], &size_i[1]);
  r_size[0] = size_i[0];
  r_size[1] = size_i[1];
}

void ED_space_image_get_aspect(SpaceImage *sima, float *r_aspx, float *r_aspy)
{
  Image *ima = sima->image;
  if ((ima == nullptr) || (ima->aspx == 0.0f || ima->aspy == 0.0f)) {
    *r_aspx = *r_aspy = 1.0;
  }
  else {
    BKE_image_get_aspect(ima, r_aspx, r_aspy);
  }
}

void ED_space_image_get_zoom(SpaceImage *sima,
                             const ARegion *region,
                             float *r_zoomx,
                             float *r_zoomy)
{
  int width, height;

  ED_space_image_get_size(sima, &width, &height);

  *r_zoomx = float(BLI_rcti_size_x(&region->winrct) + 1) /
             float(BLI_rctf_size_x(&region->v2d.cur) * width);
  *r_zoomy = float(BLI_rcti_size_y(&region->winrct) + 1) /
             float(BLI_rctf_size_y(&region->v2d.cur) * height);
}

void ED_space_image_get_uv_aspect(SpaceImage *sima, float *r_aspx, float *r_aspy)
{
  int w, h;

  ED_space_image_get_aspect(sima, r_aspx, r_aspy);
  ED_space_image_get_size(sima, &w, &h);

  *r_aspx *= float(w);
  *r_aspy *= float(h);

  if (*r_aspx < *r_aspy) {
    *r_aspy = *r_aspy / *r_aspx;
    *r_aspx = 1.0f;
  }
  else {
    *r_aspx = *r_aspx / *r_aspy;
    *r_aspy = 1.0f;
  }
}

void ED_image_get_uv_aspect(Image *ima, ImageUser *iuser, float *r_aspx, float *r_aspy)
{
  if (ima) {
    int w, h;

    BKE_image_get_aspect(ima, r_aspx, r_aspy);
    BKE_image_get_size(ima, iuser, &w, &h);

    *r_aspx *= float(w);
    *r_aspy *= float(h);
  }
  else {
    *r_aspx = 1.0f;
    *r_aspy = 1.0f;
  }
}

void ED_image_mouse_pos(SpaceImage *sima, const ARegion *region, const int mval[2], float co[2])
{
  int width, height;
  float zoomx, zoomy;

  ED_space_image_get_zoom(sima, region, &zoomx, &zoomy);
  ED_space_image_get_size(sima, &width, &height);

  /* Origin anchor in the navigation frame: the zoom scaling below is relative to the un-rotated
   * pixel of view (0, 0). The rotation is applied separately, to the input point. */
  float anchor[2];
  ui::view2d_view_to_region_navigation_fl(&region->v2d, 0.0f, 0.0f, &anchor[0], &anchor[1]);
  const int sx = int(anchor[0]);
  const int sy = int(anchor[1]);

  /* Undo the canvas rotation (screen->view) about the pivot before the axis-aligned mapping.
   * The image is displayed as `screen = Rot(-rotation) * axis_map(view)`, so the inverse
   * (screen->view) is a `+rotation` rotation about the pivot (inverse=false). */
  float p[2] = {float(mval[0]), float(mval[1])};
  ui::view2d_rotate_region_point(&region->v2d, p, false);

  co[0] = ((p[0] - sx) / zoomx) / width;
  co[1] = ((p[1] - sy) / zoomy) / height;
}

void ED_image_view_center_to_point(SpaceImage *sima, float x, float y)
{
  int width, height;
  float aspx, aspy;

  ED_space_image_get_size(sima, &width, &height);
  ED_space_image_get_aspect(sima, &aspx, &aspy);

  sima->xof = (x - 0.5f) * width * aspx;
  sima->yof = (y - 0.5f) * height * aspy;
}

void ED_image_point_pos(
    SpaceImage *sima, const ARegion *region, float x, float y, float *r_x, float *r_y)
{
  int width, height;
  float zoomx, zoomy;

  ED_space_image_get_zoom(sima, region, &zoomx, &zoomy);
  ED_space_image_get_size(sima, &width, &height);

  /* Origin anchor in the navigation frame: the zoom scaling below is relative to the un-rotated
   * pixel of view (0, 0). The rotation is applied separately, to the input point. */
  float anchor[2];
  ui::view2d_view_to_region_navigation_fl(&region->v2d, 0.0f, 0.0f, &anchor[0], &anchor[1]);
  const int sx = int(anchor[0]);
  const int sy = int(anchor[1]);

  /* Undo the canvas rotation (screen->view) about the pivot before the axis-aligned mapping.
   * See #ED_image_mouse_pos for the direction convention. */
  float p[2] = {x, y};
  ui::view2d_rotate_region_point(&region->v2d, p, false);

  *r_x = ((p[0] - sx) / zoomx) / width;
  *r_y = ((p[1] - sy) / zoomy) / height;
}

void ED_image_point_pos__reverse(SpaceImage *sima,
                                 const ARegion *region,
                                 const float co[2],
                                 float r_co[2])
{
  float zoomx, zoomy;
  int width, height;

  /* Origin anchor in the navigation frame: the zoom scaling below is relative to the un-rotated
   * pixel of view (0, 0). The rotation is applied separately, to the output point. */
  float anchor[2];
  ui::view2d_view_to_region_navigation_fl(&region->v2d, 0.0f, 0.0f, &anchor[0], &anchor[1]);
  const int sx = int(anchor[0]);
  const int sy = int(anchor[1]);
  ED_space_image_get_size(sima, &width, &height);
  ED_space_image_get_zoom(sima, region, &zoomx, &zoomy);

  r_co[0] = (co[0] * width * zoomx) + float(sx);
  r_co[1] = (co[1] * height * zoomy) + float(sy);

  /* Apply the canvas rotation (view->screen) about the pivot. The image is displayed as
   * `screen = Rot(-rotation) * axis_map(view)`, i.e. a `-rotation` rotation (inverse=true). */
  ui::view2d_rotate_region_point(&region->v2d, r_co, true);
}

bool ED_image_slot_cycle(Image *image, int direction)
{
  const int cur = image->render_slot;
  int i, slot;

  BLI_assert(ELEM(direction, -1, 1));

  int num_slots = image->renderslots.count();
  for (i = 1; i < num_slots; i++) {
    slot = (cur + ((direction == -1) ? -i : i)) % num_slots;
    if (slot < 0) {
      slot += num_slots;
    }

    RenderSlot *render_slot = BKE_image_get_renderslot(image, slot);
    if ((render_slot && render_slot->render) || slot == image->last_render_slot) {
      image->render_slot = slot;
      break;
    }
  }

  if (num_slots == 1) {
    image->render_slot = 0;
  }
  else if (i == num_slots) {
    image->render_slot = ((cur == 1) ? 0 : 1);
  }

  if (cur != image->render_slot) {
    BKE_image_partial_update_mark_full_update(image);
  }
  return (cur != image->render_slot);
}

void ED_space_image_scopes_update(const bContext *C,
                                  SpaceImage *sima,
                                  ImBuf *ibuf,
                                  bool use_view_settings)
{
  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);

  /* scope update can be expensive, don't update during paint modes */
  if (sima->mode == SI_MODE_PAINT) {
    return;
  }
  if (ob && ((ob->mode & (OB_MODE_TEXTURE_PAINT | OB_MODE_EDIT)) != 0)) {
    return;
  }

  /* We also don't update scopes of render result during render. */
  if (G.is_rendering) {
    const Image *image = sima->image;
    if (image != nullptr && ELEM(image->type, IMA_TYPE_R_RESULT, IMA_TYPE_COMPOSITE)) {
      return;
    }
  }

  BKE_scopes_update(&sima->scopes,
                    ibuf,
                    use_view_settings ? &scene->view_settings : nullptr,
                    &scene->display_settings);
}

bool ED_space_image_show_render(const SpaceImage *sima)
{
  return (sima->image && ELEM(sima->image->type, IMA_TYPE_R_RESULT, IMA_TYPE_COMPOSITE));
}

bool ED_space_image_show_paint(const SpaceImage *sima)
{
  if (ED_space_image_show_render(sima)) {
    return false;
  }

  return (sima->mode == SI_MODE_PAINT);
}

bool ED_space_image_show_mask(const SpaceImage *sima)
{
  return (sima->mode == SI_MODE_MASK);
}

bool ED_space_image_show_uvedit(const SpaceImage *sima, Object *obedit)
{
  if (sima) {
    if (ED_space_image_show_render(sima)) {
      return false;
    }
    if (sima->mode != SI_MODE_UV) {
      return false;
    }
  }

  if (obedit && obedit->type == OB_MESH) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    bool ret;

    ret = EDBM_uv_check(em);

    return ret;
  }

  return false;
}

bool ED_space_image_check_show_maskedit(SpaceImage *sima, Object *obedit)
{
  /* check editmode - this is reserved for UV editing */
  if (obedit && ED_space_image_show_uvedit(sima, obedit)) {
    return false;
  }

  return (sima->mode == SI_MODE_MASK);
}

bool ED_space_image_maskedit_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);

  if (sima) {
    const Main *bmain = CTX_data_main(C);
    Scene *scene = CTX_data_scene(C);
    ViewLayer *view_layer = CTX_data_view_layer(C);
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    Object *obedit = BKE_view_layer_edit_object_get(view_layer);
    return ED_space_image_check_show_maskedit(sima, obedit);
  }

  return false;
}

bool ED_space_image_maskedit_visible_splines_poll(bContext *C)
{
  if (!ED_space_image_maskedit_poll(C)) {
    return false;
  }

  const SpaceImage *space_image = CTX_wm_space_image(C);
  return space_image->mask_info.draw_flag & MASK_DRAWFLAG_SPLINE;
}

bool ED_space_image_paint_curve(const bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);

  if (sima && sima->mode == SI_MODE_PAINT) {
    Brush *br = BKE_paint_brush(&CTX_data_tool_settings(C)->imapaint.paint);

    if (br && (br->stroke_method == BRUSH_STROKE_CURVE)) {
      return true;
    }
  }

  return false;
}

bool ED_space_image_maskedit_mask_poll(bContext *C)
{
  if (ED_space_image_maskedit_poll(C)) {
    SpaceImage *sima = CTX_wm_space_image(C);
    return sima->mask_info.mask != nullptr;
  }

  return false;
}

bool ED_space_image_maskedit_mask_visible_splines_poll(bContext *C)
{
  if (!ED_space_image_maskedit_mask_poll(C)) {
    return false;
  }

  const SpaceImage *space_image = CTX_wm_space_image(C);
  return space_image->mask_info.draw_flag & MASK_DRAWFLAG_SPLINE;
}

/* -------------------------------------------------------------------- */
/** \name Canvas Rotation Support
 * \{ */

/**
 * Check if canvas rotation is supported in the current mode.
 * Rotation is supported in View, Paint, and Mask modes.
 * Rotation is NOT supported in UV Editor mode.
 */
bool ED_space_image_rotation_supported(const SpaceImage *sima)
{
  if (sima == nullptr) {
    return false;
  }
  return ELEM(sima->mode, SI_MODE_VIEW, SI_MODE_PAINT, SI_MODE_MASK);
}

/** \} */

bool ED_space_image_cursor_poll(bContext *C)
{
  return ED_operator_uvedit_space_image(C) || ED_space_image_maskedit_poll(C) ||
         ED_space_image_paint_curve(C);
}

bool ED_space_image_region_cursor_poll(bContext *C)
{
  const ARegion *region = CTX_wm_region(C);
  if (!(region && region->regiontype == RGN_TYPE_WINDOW)) {
    return false;
  }
  return ED_space_image_cursor_poll(C);
}

}  // namespace blender
