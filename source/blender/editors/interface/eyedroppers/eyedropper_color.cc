/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Eyedropper (RGB Color)
 *
 * Defines:
 * - #UI_OT_eyedropper_color
 */

#include <climits>

#include "MEM_guardedalloc.h"

#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "BKE_context.hh"
#include "BKE_cryptomatte.h"
#include "BKE_image.hh"
#include "BKE_layer.hh"
#include "BKE_material.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "NOD_composite.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"
#include "wm_window.hh"

#include "interface_intern.hh"

#include "ED_clip.hh"
#include "ED_image.hh"
#include "ED_node.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "RE_pipeline.h"

#include "eyedropper_intern.hh"

namespace blender::ui {

/** Interval of the timer that tracks the cursor outside of the Blender windows, in seconds. */
static constexpr double EYE_PREVIEW_UPDATE_INTERVAL = 1.0 / 30.0;

/**
 * Whether the color under a cursor that left all Blender windows can be previewed continuously.
 *
 * Back-ends without desktop sampling have nothing to show, and back-ends where sampling opens a
 * system provided picker must not be polled, as every call blocks until the user confirms.
 */
static bool eyedropper_desktop_preview_supported()
{
  const eWM_CapabilitiesFlag capabilities = WM_capabilities_flag();
  return (capabilities & WM_CAPABILITY_DESKTOP_SAMPLE) &&
         (capabilities & WM_CAPABILITY_DESKTOP_SAMPLE_CONTINUOUS);
}

struct Eyedropper {
  const ColorManagedDisplay *display = nullptr;

  PointerRNA ptr = {};
  PropertyRNA *prop = nullptr;
  int index = 0;
  bool is_undo = false;

  bool is_set = false;
  float init_col[3] = {}; /* for resetting on cancel */

  bool accum_start = false; /* has mouse been pressed */
  float accum_col[3] = {};
  int accum_tot = 0;

  /**
   * The window the operator was invoked from. Note that an `event_xy` is always in the coordinate
   * space of the window that produced the event, which is not necessarily this one, see
   * #WM_event_add_modal_handler_cross_window.
   */
  wmWindow *cb_win = nullptr;
  /* The preview is drawn by a window-level callback, not by a region paint cursor. */
  wmWindow *draw_win = nullptr;
  void *draw_handle = nullptr;
  /** Cursor position in the pixel space of #draw_win. */
  int draw_xy[2] = {};
  char sample_text[MAX_NAME] = {};
  /** The color under the cursor, in scene linear space. */
  float current_col[3] = {};

  bNode *crypto_node = nullptr;
  CryptomatteSession *cryptomatte_session = nullptr;
  ViewportColorSampleSession *viewport_session = nullptr;

  /* Keeps the preview up to date whenever motion events do not reach this operator. */
  wmTimer *timer = nullptr;
  /**
   * Whether a motion event was handled since the last timer tick,
   * see #eyedropper_modal_handle_timer.
   */
  bool motion_handled = false;
  /** Cursor position of the last timer poll, in the pixel space of #cb_win. */
  int timer_xy[2] = {INT_MIN, INT_MIN};

  /* Windows this operator set the modal cursor for, so only those get restored on exit. */
  Vector<wmWindow *> cursor_windows;
  /** Whether #WM_paint_cursor_suppress_push was called, so the pop stays balanced. */
  bool paintcursors_suppressed = false;
};

/**
 * The modal cursor is stored per window, but the cursor can be moved over any Blender window while
 * sampling, so set it everywhere. Otherwise the other windows keep showing their own cursor, for
 * example the one of the active tool.
 *
 * Windows where another modal operator already took the cursor are left alone, matching the
 * convention used by the view navigation operators.
 */
static void eyedropper_cursor_modal_set(bContext *C, Eyedropper *eye)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  for (wmWindow &win : wm->windows) {
    /* The window the operator was invoked from always shows the eyedropper cursor, the others only
     * when no other modal operator is using their cursor. */
    if ((&win != eye->cb_win) && !WM_cursor_modal_is_set_ok(&win)) {
      continue;
    }
    WM_cursor_modal_set(&win, WM_CURSOR_EYEDROPPER);
    eye->cursor_windows.append(&win);
  }

  /* The active tool's paint cursor overlay is drawn independently of the modal cursor set above,
   * so it would otherwise keep showing (e.g. the brush circle) while sampling a color. */
  WM_paint_cursor_suppress_push();
  eye->paintcursors_suppressed = true;
}

static void eyedropper_cursor_modal_restore(bContext *C, Eyedropper *eye)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  for (wmWindow *cursor_win : eye->cursor_windows) {
    /* The window may have been closed while the operator was running. */
    if (BLI_findindex(&wm->windows, cursor_win) != -1) {
      WM_cursor_modal_restore(cursor_win);
    }
  }
  eye->cursor_windows.clear();

  /* The #exec variant of this operator never sets the modal cursor, so this may run without a
   * matching #eyedropper_cursor_modal_set. */
  if (eye->paintcursors_suppressed) {
    WM_paint_cursor_suppress_pop();
    eye->paintcursors_suppressed = false;
  }
}

static void eyedropper_draw_cb(const wmWindow *window, void *arg)
{
  Eyedropper *eye = static_cast<Eyedropper *>(arg);

  if (window != eye->draw_win) {
    return;
  }

  /* Draw color swatch for regular color picking. */
  if (!eye->crypto_node) {
    float display_col[3];
    copy_v3_v3(display_col, eye->current_col);

    /* Convert from linear rgb space to display space for preview. */
    if (eye->display) {
      IMB_colormanagement_scene_linear_to_display_v3(display_col, eye->display);
    }

    eyedropper_draw_cursor_color_window(window, eye->draw_xy, display_col);
  }

  /* Draw text for cryptomatte. */
  eyedropper_draw_cursor_text_region(eye->draw_xy, eye->sample_text);
}

static void eyedropper_draw_window_set(Eyedropper *eye, wmWindow *window)
{
  if (eye->draw_win == window) {
    return;
  }

  wmWindow *old_window = eye->draw_win;
  if (eye->draw_handle && old_window) {
    WM_draw_cb_exit(old_window, eye->draw_handle);
  }

  eye->draw_win = window;
  eye->draw_handle = window ? WM_draw_cb_activate(window, eyedropper_draw_cb, eye) : nullptr;

  if (old_window) {
    bScreen *old_screen = WM_window_get_active_screen(old_window);
    if (old_screen) {
      old_screen->do_draw = true;
    }
  }
  if (window) {
    bScreen *screen = WM_window_get_active_screen(window);
    if (screen) {
      screen->do_draw = true;
    }
  }
}

static bool eyedropper_init(bContext *C, wmOperator *op)
{
  Eyedropper *eye = MEM_new<Eyedropper>(__func__);

  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "prop_data_path");
  if (prop && RNA_property_is_set(op->ptr, prop)) {
    std::string prop_data_path = RNA_string_get(op->ptr, "prop_data_path");
    if (prop_data_path.empty()) {
      MEM_delete(eye);
      return false;
    }
    PointerRNA ctx_ptr = RNA_pointer_create_discrete(nullptr, RNA_Context, C);
    if (!RNA_path_resolve(&ctx_ptr, prop_data_path.c_str(), &eye->ptr, &eye->prop)) {
      BKE_reportf(op->reports, RPT_ERROR, "Could not resolve path '%s'", prop_data_path.c_str());
      MEM_delete(eye);
      return false;
    }
    eye->is_undo = true;
  }
  else {
    Button *but = context_active_but_prop_get(C, &eye->ptr, &eye->prop, &eye->index);
    if (but != nullptr) {
      eye->is_undo = button_flag_is_set(but, BUT_UNDO);
    }
  }

  const enum PropertySubType prop_subtype = eye->prop ? RNA_property_subtype(eye->prop) :
                                                        PropertySubType(0);

  if ((eye->ptr.data == nullptr) || (eye->prop == nullptr) ||
      (RNA_property_editable(&eye->ptr, eye->prop) == false) ||
      (RNA_property_array_length(&eye->ptr, eye->prop) < 3) ||
      (RNA_property_type(eye->prop) != PROP_FLOAT) ||
      (ELEM(prop_subtype, PROP_COLOR, PROP_COLOR_GAMMA) == 0))
  {
    MEM_delete(eye);
    return false;
  }
  op->customdata = eye;

  float col[4];
  RNA_property_float_get_array_at_most(&eye->ptr, eye->prop, col, ARRAY_SIZE(col));
  if (eye->ptr.type == RNA_CompositorNodeCryptomatteV2) {
    BKE_main_view_layers_synced_ensure(CTX_data_main(C));
    eye->crypto_node = static_cast<bNode *>(eye->ptr.data);
    eye->cryptomatte_session = ntreeCompositCryptomatteSession(eye->crypto_node);
  }

  /* Window draw callbacks run after all regions, in full window pixel space. */
  eye->cb_win = CTX_wm_window(C);
  eyedropper_draw_window_set(eye, eye->cb_win);

  /* #WM_event_add_modal_handler_cross_window (used below) only forwards events another window did
   * not use itself, and events are only generated while the cursor is inside a Blender window. So
   * this operator stops receiving motion once the cursor leaves all Blender windows, or moves into
   * a window that consumes the motion itself. Poll the cursor position from a timer to keep the
   * preview up to date in those cases. */
  wmWindowManager *wm = CTX_wm_manager(C);
  eye->timer = WM_event_timer_add(wm, eye->cb_win, TIMER, EYE_PREVIEW_UPDATE_INTERVAL);

  Scene *scene = CTX_data_scene(C);
  const char *display_device = scene->display_settings.display_device;
  eye->display = IMB_colormanagement_display_get_named(display_device);

  /* Gamma corrected properties store display space colors, convert to the scene linear space
   * used by #eyedropper_color_set. */
  if (prop_subtype == PROP_COLOR_GAMMA && eye->display) {
    IMB_colormanagement_display_to_scene_linear_v3(col, eye->display);
  }
  copy_v3_v3(eye->init_col, col);

  return true;
}

static void eyedropper_exit(bContext *C, wmOperator *op)
{
  Eyedropper *eye = static_cast<Eyedropper *>(op->customdata);
  /* Remove timer. */
  if (eye->timer) {
    wmWindowManager *wm = CTX_wm_manager(C);
    WM_event_timer_remove(wm, eye->cb_win, eye->timer);
    eye->timer = nullptr;
  }

  eyedropper_cursor_modal_restore(C, eye);
  ED_workspace_status_text(C, nullptr);

  if (eye->draw_handle && eye->draw_win) {
    WM_draw_cb_exit(eye->draw_win, eye->draw_handle);
    bScreen *screen = WM_window_get_active_screen(eye->draw_win);
    if (screen) {
      screen->do_draw = true;
    }
    eye->draw_handle = nullptr;
    eye->draw_win = nullptr;
  }

  if (eye->cryptomatte_session) {
    BKE_cryptomatte_free(eye->cryptomatte_session);
    eye->cryptomatte_session = nullptr;
  }

  if (eye->viewport_session) {
    MEM_delete(eye->viewport_session);
    eye->viewport_session = nullptr;
  }

  op->customdata = nullptr;
  MEM_delete(eye);
}

/* *** eyedropper_color_ helper functions *** */

static bool eyedropper_cryptomatte_sample_view3d_fl(bContext *C,
                                                    const char *type_name,
                                                    const int mval[2],
                                                    float r_col[3])
{
  int material_slot = 0;
  Object *object = ED_view3d_give_material_slot_under_cursor(C, mval, &material_slot);
  if (!object) {
    return false;
  }

  const ID *id = nullptr;
  if (StringRef(type_name).endswith(RE_PASSNAME_CRYPTOMATTE_OBJECT)) {
    id = &object->id;
  }
  else if (StringRef(type_name).endswith(RE_PASSNAME_CRYPTOMATTE_MATERIAL)) {
    Material *material = BKE_object_material_get(object, material_slot);
    if (!material) {
      return false;
    }
    id = &material->id;
  }

  if (!id) {
    return false;
  }

  const char *name = &id->name[2];
  const int name_length = BLI_strnlen(name, MAX_NAME - 2);
  uint32_t cryptomatte_hash = BKE_cryptomatte_hash(name, name_length);
  r_col[0] = BKE_cryptomatte_hash_to_float(cryptomatte_hash);
  return true;
}

static bool eyedropper_cryptomatte_sample_renderlayer_fl(RenderLayer *render_layer,
                                                         const char *prefix,
                                                         const float fpos[2],
                                                         float r_col[3])
{
  if (!render_layer) {
    return false;
  }

  const int render_layer_name_len = STRNLEN(render_layer->name);
  if (strncmp(prefix, render_layer->name, render_layer_name_len) != 0) {
    return false;
  }

  const int prefix_len = strlen(prefix);
  if (prefix_len <= render_layer_name_len + 1) {
    return false;
  }

  /* RenderResult from images can have no render layer name. */
  const char *render_pass_name_prefix = render_layer_name_len ?
                                            prefix + 1 + render_layer_name_len :
                                            prefix;

  for (RenderPass &render_pass : render_layer->passes) {
    if (STRPREFIX(render_pass.name, render_pass_name_prefix) &&
        !STREQLEN(render_pass.name, render_pass_name_prefix, sizeof(render_pass.name)))
    {
      BLI_assert(render_pass.channels == 4);

      /* Pass was allocated but not rendered yet. */
      if (!render_pass.ibuf) {
        return false;
      }

      const int x = int(fpos[0] * render_pass.rectx);
      const int y = int(fpos[1] * render_pass.recty);
      const int offset = 4 * (y * render_pass.rectx + x);
      zero_v3(r_col);
      r_col[0] = render_pass.ibuf->float_data()[offset];
      return true;
    }
  }

  return false;
}

static bool eyedropper_cryptomatte_sample_render_fl(const bNode *node,
                                                    const char *prefix,
                                                    const float fpos[2],
                                                    float r_col[3])
{
  bool success = false;
  Scene *scene = id_cast<Scene *>(node->id);
  BLI_assert(GS(scene->id.name) == ID_SCE);
  Render *re = RE_GetSceneRender(scene);

  if (re) {
    RenderResult *rr = RE_AcquireResultRead(re);
    if (rr) {
      for (ViewLayer &view_layer : scene->view_layers) {
        RenderLayer *render_layer = RE_GetRenderLayer(rr, view_layer.name);
        success = eyedropper_cryptomatte_sample_renderlayer_fl(render_layer, prefix, fpos, r_col);
        if (success) {
          break;
        }
      }
    }
    RE_ReleaseResult(re);
  }
  return success;
}

static bool eyedropper_cryptomatte_sample_image_fl(bContext *C,
                                                   const bNode *node,
                                                   NodeCryptomatte *crypto,
                                                   const char *prefix,
                                                   const float fpos[2],
                                                   float r_col[3])
{
  bool success = false;
  Image *image = id_cast<Image *>(node->id);
  BLI_assert((image == nullptr) || (GS(image->id.name) == ID_IM));

  /* Compute the effective frame number of the image if it was animated. */
  Scene *scene = CTX_data_scene(C);
  ImageUser image_user_for_frame = crypto->iuser;
  BKE_image_user_frame_calc(image, &image_user_for_frame, scene->r.cfra);

  if (image && image->type == IMA_TYPE_MULTILAYER) {
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &image_user_for_frame, nullptr);
    if (image->rr) {
      for (RenderLayer &render_layer : image->rr->layers) {
        success = eyedropper_cryptomatte_sample_renderlayer_fl(&render_layer, prefix, fpos, r_col);
        if (success) {
          break;
        }
      }
    }
    BKE_image_release_ibuf(image, ibuf, nullptr);
  }
  return success;
}

static bool eyedropper_cryptomatte_sample_fl(bContext *C,
                                             Eyedropper *eye,
                                             const int event_xy[2],
                                             float r_col[3])
{
  bNode *node = eye->crypto_node;
  NodeCryptomatte *crypto = node ? (static_cast<NodeCryptomatte *>(node->storage)) : nullptr;

  if (!crypto) {
    return false;
  }

  ScrArea *area = nullptr;

  int event_xy_win[2];
  /* `event_xy` is in the coordinate space of the window that produced the event, which is not
   * necessarily #Eyedropper.cb_win once events are forwarded from another window, see
   * #WM_event_add_modal_handler_cross_window. */
  wmWindow *win = WM_window_find_under_cursor(CTX_wm_window(C), event_xy, event_xy_win);
  if (win) {
    bScreen *screen = WM_window_get_active_screen(win);
    area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, event_xy_win);
  }

  if (!area || !ELEM(area->spacetype, SPACE_IMAGE, SPACE_NODE, SPACE_CLIP, SPACE_VIEW3D)) {
    return false;
  }

  ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event_xy_win);

  if (!region) {
    return false;
  }

  const int mval[2] = {
      event_xy_win[0] - region->winrct.xmin,
      event_xy_win[1] - region->winrct.ymin,
  };
  float fpos[2] = {-1.0f, -1.0};
  switch (area->spacetype) {
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      ED_space_image_get_position(sima, region, mval, fpos);
      break;
    }
    case SPACE_NODE: {
      Main *bmain = CTX_data_main(C);
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      ED_space_node_get_position(bmain, snode, region, mval, fpos);
      break;
    }
    case SPACE_CLIP: {
      SpaceClip *sc = static_cast<SpaceClip *>(area->spacedata.first);
      ED_space_clip_get_position(sc, region, mval, fpos);
      break;
    }
    default: {
      break;
    }
  }

  if (area->spacetype != SPACE_VIEW3D &&
      (fpos[0] < 0.0f || fpos[1] < 0.0f || fpos[0] >= 1.0f || fpos[1] >= 1.0f))
  {
    return false;
  }

  /* CMP_NODE_CRYPTOMATTE_SOURCE_RENDER and CMP_NODE_CRYPTOMATTE_SOURCE_IMAGE require a referenced
   * image/scene to work properly. */
  if (!node->id) {
    return false;
  }

  ED_region_tag_redraw(region);

  /* TODO(jbakker): Migrate this file to cc and use std::string as return param. */
  char prefix[MAX_NAME + 1];
  ntreeCompositCryptomatteLayerPrefix(node, prefix, sizeof(prefix) - 1);
  prefix[MAX_NAME] = '\0';

  if (area->spacetype == SPACE_VIEW3D) {
    wmWindow *win_prev = CTX_wm_window(C);
    ScrArea *area_prev = CTX_wm_area(C);
    ARegion *region_prev = CTX_wm_region(C);

    CTX_wm_window_set(C, win);
    CTX_wm_area_set(C, area);
    CTX_wm_region_set(C, region);

    const bool success = eyedropper_cryptomatte_sample_view3d_fl(C, prefix, mval, r_col);

    CTX_wm_window_set(C, win_prev);
    CTX_wm_area_set(C, area_prev);
    CTX_wm_region_set(C, region_prev);

    return success;
  }
  if (node->custom1 == CMP_NODE_CRYPTOMATTE_SOURCE_RENDER) {
    return eyedropper_cryptomatte_sample_render_fl(node, prefix, fpos, r_col);
  }
  if (node->custom1 == CMP_NODE_CRYPTOMATTE_SOURCE_IMAGE) {
    return eyedropper_cryptomatte_sample_image_fl(C, node, crypto, prefix, fpos, r_col);
  }
  return false;
}

bool eyedropper_color_sample_fl(bContext *C,
                                Eyedropper *eye,
                                const int event_xy[2],
                                float r_col[3])
{
  ScrArea *area = nullptr;

  int event_xy_win[2];
  /* `event_xy` is in the coordinate space of the window that produced the event, which is not
   * necessarily #Eyedropper.cb_win once events are forwarded from another window, see
   * #WM_event_add_modal_handler_cross_window. */
  wmWindow *win = WM_window_find_under_cursor(CTX_wm_window(C), event_xy, event_xy_win);

  if (win) {
    bScreen *screen = WM_window_get_active_screen(win);
    area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, event_xy_win);
  }

  if (area) {
    ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event_xy_win);
    if (region) {
      const int mval[2] = {
          event_xy_win[0] - region->winrct.xmin,
          event_xy_win[1] - region->winrct.ymin,
      };
      if (area->spacetype == SPACE_IMAGE) {
        SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
        if (ED_space_image_color_sample(sima, region, mval, r_col, nullptr)) {
          return true;
        }
      }
      else if (area->spacetype == SPACE_NODE) {
        SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
        Main *bmain = CTX_data_main(C);
        if (ED_space_node_color_sample(bmain, snode, region, mval, r_col)) {
          return true;
        }
      }
      else if (area->spacetype == SPACE_CLIP) {
        SpaceClip *sc = static_cast<SpaceClip *>(area->spacedata.first);
        if (ED_space_clip_color_sample(sc, region, mval, r_col)) {
          return true;
        }
      }
      else if (eye != nullptr && area->spacetype == SPACE_VIEW3D) {
        /* Viewport color picking involves a fairly expensive operation to copy the GPU viewport
         * back to the CPU, so to support smooth dragging with the eyedropper, we keep the copy
         * around for the entire operation. */
        if (eye->viewport_session == nullptr) {
          eye->viewport_session = MEM_new<ViewportColorSampleSession>("viewport_session");
          eye->viewport_session->init(region);
        }
        if (eye->viewport_session->sample(mval, r_col)) {
          return true;
        }
      }
    }
  }

  /* Other areas within a Blender window.
   *
   * NOTE: #WM_window_find_under_cursor also matches the window decorations (title bar, resize
   * border), so `event_xy_win` can be outside of the drawable area. Reading a pixel there would
   * make the GPU back-end copy a region that lies outside of the frame-buffer image, which resets
   * the driver on Vulkan. Fall through to desktop sampling in that case. */
  if (win) {
    const int2 win_size = WM_window_native_pixel_size(win);
    if (uint(event_xy_win[0]) < uint(win_size[0]) && uint(event_xy_win[1]) < uint(win_size[1])) {
      if (!WM_window_pixels_read_sample(C, win, event_xy_win, r_col)) {
        WM_window_pixels_read_sample_from_offscreen(C, win, event_xy_win, r_col);
      }
      const char *display_device = CTX_data_scene(C)->display_settings.display_device;
      const ColorManagedDisplay *display = IMB_colormanagement_display_get_named(display_device);
      IMB_colormanagement_display_to_scene_linear_v3(r_col, display);
      return true;
    }
  }

  /* Outside the Blender window if we support it. */
  if (WM_capabilities_flag() & WM_CAPABILITY_DESKTOP_SAMPLE) {
    if (WM_desktop_cursor_sample_read(r_col)) {
      IMB_colormanagement_srgb_to_scene_linear_v3(r_col, r_col);
      return true;
    }
  }

  zero_v3(r_col);
  return false;
}

/* sets the sample color RGB, maintaining A */
static void eyedropper_color_set(bContext *C, Eyedropper *eye, const float col[3])
{
  float col_conv[4];

  /* to maintain alpha */
  RNA_property_float_get_array_at_most(&eye->ptr, eye->prop, col_conv, ARRAY_SIZE(col_conv));

  /* Sampled colors are in scene linear space. Only gamma corrected properties store the color in
   * display space, others (shader node sockets for example) are already scene linear. */
  copy_v3_v3(col_conv, col);
  if (RNA_property_subtype(eye->prop) == PROP_COLOR_GAMMA && eye->display) {
    IMB_colormanagement_scene_linear_to_display_v3(col_conv, eye->display);
  }

  RNA_property_float_set_array_at_most(&eye->ptr, eye->prop, col_conv, ARRAY_SIZE(col_conv));
  eye->is_set = true;

  RNA_property_update(C, &eye->ptr, eye->prop);
}

static void eyedropper_color_sample(bContext *C, Eyedropper *eye, const int event_xy[2])
{
  /* Accumulate color. */
  float col[3];
  if (eye->crypto_node) {
    if (!eyedropper_cryptomatte_sample_fl(C, eye, event_xy, col)) {
      return;
    }
  }
  else {
    if (!eyedropper_color_sample_fl(C, eye, event_xy, col)) {
      return;
    }
  }

  if (!eye->crypto_node) {
    add_v3_v3(eye->accum_col, col);
    eye->accum_tot++;
  }
  else {
    copy_v3_v3(eye->accum_col, col);
    eye->accum_tot = 1;
  }

  /* Apply to property. */
  float accum_col[3];
  if (eye->accum_tot > 1) {
    mul_v3_v3fl(accum_col, eye->accum_col, 1.0f / float(eye->accum_tot));
  }
  else {
    copy_v3_v3(accum_col, eye->accum_col);
  }
  eyedropper_color_set(C, eye, accum_col);
}

/**
 * Sample the color under the cursor once, without any side effect on the draw state.
 *
 * The sampled value is also reused for the cryptomatte name lookup so a single preview update
 * never samples the same pixel twice (each sample can trigger a GPU read-back).
 */
static void eyedropper_preview_state_update(bContext *C, Eyedropper *eye, const int event_xy[2])
{
  float col[3];

  eye->sample_text[0] = '\0';

  if (eye->crypto_node) {
    if (!eyedropper_cryptomatte_sample_fl(C, eye, event_xy, col)) {
      return;
    }
    if (eye->cryptomatte_session) {
      BKE_cryptomatte_find_name(
          eye->cryptomatte_session, col[0], eye->sample_text, sizeof(eye->sample_text));
      eye->sample_text[sizeof(eye->sample_text) - 1] = '\0';
    }
  }
  else if (!eyedropper_color_sample_fl(C, eye, event_xy, col)) {
    return;
  }

  /* The preview uses the sampled color directly, the display conversion happens while drawing. */
  copy_v3_v3(eye->current_col, col);
}

static void eyedropper_preview_tag_redraw(Eyedropper *eye)
{
  if (eye->draw_win == nullptr) {
    return;
  }
  bScreen *screen = WM_window_get_active_screen(eye->draw_win);
  if (screen) {
    /* Only the window composite has to be repeated, regions keep their cached draw. */
    screen->do_draw = true;
  }
}

/**
 * Refresh the preview for a cursor at `event_xy`, in the coordinate space of #Eyedropper.cb_win.
 */
static void eyedropper_update_preview(bContext *C, Eyedropper *eye, const int event_xy[2])
{
  int draw_xy_prev[2];
  copy_v2_v2_int(draw_xy_prev, eye->draw_xy);
  float col_prev[3];
  copy_v3_v3(col_prev, eye->current_col);

  int event_xy_win[2] = {};
  /* `event_xy` is in the coordinate space of the window that produced the event, which is not
   * necessarily #Eyedropper.cb_win once events are forwarded from another window, see
   * #WM_event_add_modal_handler_cross_window. */
  wmWindow *win = WM_window_find_under_cursor(CTX_wm_window(C), event_xy, event_xy_win);

  if (win) {
    eyedropper_draw_window_set(eye, win);
    copy_v2_v2_int(eye->draw_xy, event_xy_win);
    eyedropper_preview_state_update(C, eye, event_xy);
  }
  else if (!eyedropper_desktop_preview_supported()) {
    /* Sampling the desktop is either unsupported or opens a system provided picker that blocks
     * until the user confirms. There is nothing to show, so hide the preview instead of leaving
     * the color of the last sample on screen. */
    eyedropper_draw_window_set(eye, nullptr);
    return;
  }
  else {
    /* Outside of any Blender window the preview is clamped to the edge of the operator's own
     * window, so keep the callback on that window instead of removing and re-adding it. */
    eyedropper_draw_window_set(eye, eye->cb_win);
    if (!wm_cursor_position_get(eye->cb_win, &eye->draw_xy[0], &eye->draw_xy[1])) {
      copy_v2_v2_int(eye->draw_xy, event_xy);
    }
    eyedropper_preview_state_update(C, eye, event_xy);
  }

  /* The timer polls at a fixed rate, only redraw when the preview actually changed.
   * A change of the drawn window is tagged by #eyedropper_draw_window_set. */
  const bool draw_xy_changed = (draw_xy_prev[0] != eye->draw_xy[0]) ||
                               (draw_xy_prev[1] != eye->draw_xy[1]);
  if (draw_xy_changed || !equals_v3v3(col_prev, eye->current_col)) {
    eyedropper_preview_tag_redraw(eye);
  }
}

static void eyedropper_cancel(bContext *C, wmOperator *op)
{
  Eyedropper *eye = static_cast<Eyedropper *>(op->customdata);
  if (eye == nullptr) {
    /* Already exited, this can happen when handlers are removed after the operator finished. */
    return;
  }
  if (eye->is_set) {
    eyedropper_color_set(C, eye, eye->init_col);
  }

  eyedropper_exit(C, op);
}

static void eyedropper_modal_handle_mousemove(bContext *C,
                                              wmOperator *op,
                                              const wmEvent *event,
                                              Eyedropper *eye)
{
  if (eye->accum_start) {
    /* Button is pressed so keep sampling. */
    eyedropper_color_sample(C, eye, event->xy);
    WorkspaceStatus status(C);
    status.item(TIP_("Drag to continue sampling, release when done"), ICON_MOUSE_MOVE);
  }
  else {
    WorkspaceStatus status(C);
    status.opmodal(IFACE_("Confirm"), op->type, EYE_MODAL_SAMPLE_CONFIRM);
    status.opmodal(IFACE_("Cancel"), op->type, EYE_MODAL_CANCEL);
    if ((WM_capabilities_flag() & WM_CAPABILITY_DESKTOP_SAMPLE) &&
        !eyedropper_desktop_preview_supported())
    {
      /* Sampling outside of a Blender window opens a system provided picker, which cannot be
       * driven by the cursor and has to be requested explicitly. */
      status.item(TIP_("Press 'Enter' to sample outside of a Blender window"), ICON_INFO);
    }
  }

  eye->motion_handled = true;
  eyedropper_update_preview(C, eye, event->xy);
}

/**
 * Fallback for the cases where motion events do not reach this operator, see the timer setup in
 * #eyedropper_init. Skipped while motion events are coming in, so the pixel under the cursor is
 * never sampled twice for the same movement.
 */
static void eyedropper_modal_handle_timer(bContext *C, Eyedropper *eye)
{
  if (eye->motion_handled) {
    eye->motion_handled = false;
    return;
  }

  int mouse_xy[2];
  if (!wm_cursor_position_get(eye->cb_win, &mouse_xy[0], &mouse_xy[1])) {
    return;
  }

  /* Sampling reads back a pixel from the GPU, which stalls the render pipeline. Only do so when
   * the cursor actually moved, otherwise a resting cursor would keep the timer sampling at the
   * poll rate for as long as the operator runs. */
  if ((mouse_xy[0] == eye->timer_xy[0]) && (mouse_xy[1] == eye->timer_xy[1])) {
    return;
  }
  copy_v2_v2_int(eye->timer_xy, mouse_xy);

  eyedropper_update_preview(C, eye, mouse_xy);
}

/* Main modal status check. */
static wmOperatorStatus eyedropper_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  Eyedropper *eye = static_cast<Eyedropper *>(op->customdata);

  /* handle modal keymap */
  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case EYE_MODAL_CANCEL:
        eyedropper_cancel(C, op);
        return OPERATOR_CANCELLED;
      case EYE_MODAL_SAMPLE_CONFIRM: {
        const bool is_undo = eye->is_undo;
        if (eye->accum_tot == 0) {
          eyedropper_color_sample(C, eye, event->xy);
        }
        eyedropper_exit(C, op);
        /* Could support finished & undo-skip. */
        return is_undo ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      }
      case EYE_MODAL_SAMPLE_BEGIN:
        /* enable accum and make first sample */
        eye->accum_start = true;
        eyedropper_color_sample(C, eye, event->xy);
        break;
      case EYE_MODAL_SAMPLE_RESET:
        eye->accum_tot = 0;
        zero_v3(eye->accum_col);
        eyedropper_color_sample(C, eye, event->xy);
        break;
    }
  }
  else if (ISMOUSE_MOTION(event->type)) {
    eyedropper_modal_handle_mousemove(C, op, event, eye);
  }
  else if (eye->timer && event->type == TIMER && event->customdata == eye->timer) {
    eyedropper_modal_handle_timer(C, eye);
  }

  return OPERATOR_RUNNING_MODAL;
}

/* Modal Operator init */
static wmOperatorStatus eyedropper_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  /* init */
  if (eyedropper_init(C, op)) {
    wmWindow *win = CTX_wm_window(C);
    /* Workaround for de-activating the button clearing the cursor, see #76794 */
    context_active_but_clear(C, win, CTX_wm_region(C));
    eyedropper_cursor_modal_set(C, static_cast<Eyedropper *>(op->customdata));

    /* Add temp handler. Sampling should keep working after the user switches to another window
     * without confirming or canceling first, see #WM_event_add_modal_handler_cross_window. */
    WM_event_add_modal_handler_cross_window(C, op);

    return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_PASS_THROUGH;
}

/* Repeat operator */
static wmOperatorStatus eyedropper_exec(bContext *C, wmOperator *op)
{
  /* init */
  if (eyedropper_init(C, op)) {

    /* do something */

    /* cleanup */
    eyedropper_exit(C, op);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_PASS_THROUGH;
}

static bool eyedropper_poll(bContext *C)
{
  /* Actual test for active button happens later, since we don't
   * know which one is active until mouse over. */
  return (CTX_wm_window(C) != nullptr);
}

void UI_OT_eyedropper_color(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Eyedropper";
  ot->idname = "UI_OT_eyedropper_color";
  ot->description = "Sample a color from the Blender window to store in a property";

  /* API callbacks. */
  ot->invoke = eyedropper_invoke;
  ot->modal = eyedropper_modal;
  ot->cancel = eyedropper_cancel;
  ot->exec = eyedropper_exec;
  ot->poll = eyedropper_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO | OPTYPE_BLOCKING | OPTYPE_INTERNAL;

  /* Paths relative to the context. */
  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna,
                        "prop_data_path",
                        nullptr,
                        0,
                        "Data Path",
                        "Path of property to be set with the depth");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

}  // namespace blender::ui
