/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../../space_image/image_runtime.hh"
#include "../paint_intern.hh"
#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"

namespace blender {

static uint64_t image_paint_warp_settings_revision = 0;

void ED_image_paint_select_warp_settings_revision_bump()
{
  image_paint_warp_settings_revision++;
}

/* -------------------------------------------------------------------- */
/** \name Bilinear cell math
 * \{ */

float2 warp_bilinear_eval(const float2 &p00,
                          const float2 &p10,
                          const float2 &p01,
                          const float2 &p11,
                          const float u,
                          const float v)
{
  const float2 a = p11 - p10 - p01 + p00;
  const float2 b = p10 - p00;
  const float2 c = p01 - p00;
  return p00 + b * u + c * v + a * (u * v);
}

/** Uniform Catmull-Rom basis through p1, p2 (with neighbors p0, p3) at t in [0, 1]. */
static float2 warp_catmull_rom_1d(const float2 &p0,
                                  const float2 &p1,
                                  const float2 &p2,
                                  const float2 &p3,
                                  const float t)
{
  const float t2 = t * t;
  const float t3 = t2 * t;
  return 0.5f * (2.0f * p1 + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                 (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

float2 warp_grid_eval_smooth(const float2 *pts, const int grid_size, float gx, float gy)
{
  gx = std::clamp(gx, 0.0f, float(grid_size - 1));
  gy = std::clamp(gy, 0.0f, float(grid_size - 1));
  /* Locate the containing cell; clamp so a valid right/bottom neighbor exists and the local
   * parameter reaches exactly 1.0 at the far edge (matches warp_grid_eval's linear branch). */
  const int cell_x = std::clamp(int(gx), 0, grid_size - 2);
  const int cell_y = std::clamp(int(gy), 0, grid_size - 2);
  const float u = gx - float(cell_x);
  const float v = gy - float(cell_y);

  /* Fetch one control point of a given (already in-range) row, extrapolating linearly one step
   * past either horizontal edge: p[-1] = 2*p[0] - p[1]. */
  auto fetch_x = [&](const int row_base, const int x) -> float2 {
    if (x < 0) {
      return 2.0f * pts[row_base + 0] - pts[row_base + 1];
    }
    if (x > grid_size - 1) {
      return 2.0f * pts[row_base + (grid_size - 1)] - pts[row_base + (grid_size - 2)];
    }
    return pts[row_base + x];
  };

  /* Gather the 4x4 neighborhood: extrapolate in x within each row, then in y for the (at most
   * one) out-of-range row on each side, so affine fields are reproduced exactly at all borders. */
  float2 grid4[4][4];
  for (int j = 0; j < 4; j++) {
    const int row_base = std::clamp(cell_y - 1 + j, 0, grid_size - 1) * grid_size;
    for (int i = 0; i < 4; i++) {
      grid4[j][i] = fetch_x(row_base, cell_x - 1 + i);
    }
  }
  if (cell_y - 1 < 0) {
    for (int i = 0; i < 4; i++) {
      grid4[0][i] = 2.0f * grid4[1][i] - grid4[2][i];
    }
  }
  if (cell_y + 2 > grid_size - 1) {
    for (int i = 0; i < 4; i++) {
      grid4[3][i] = 2.0f * grid4[2][i] - grid4[1][i];
    }
  }

  float2 cols[4];
  for (int j = 0; j < 4; j++) {
    cols[j] = warp_catmull_rom_1d(grid4[j][0], grid4[j][1], grid4[j][2], grid4[j][3], u);
  }
  return warp_catmull_rom_1d(cols[0], cols[1], cols[2], cols[3], v);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Fragment capture
 * \{ */

bool image_select_warp_extract(wmOperator *op,
                               Image *ima,
                               const ImageUser &base_iuser,
                               const int tile_number,
                               SelectionTileFragment *r_fragment)
{
  int tight_min[2], tight_max[2];
  if (!BKE_image_paint_selection_mask_bounds(ima, tile_number, tight_min, tight_max)) {
    BKE_report(op->reports, RPT_ERROR, "Selection is empty");
    return false;
  }

  ImageUser tile_iuser = base_iuser;
  tile_iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &tile_iuser, &lock);
  if (!ibuf || (!ibuf->float_buffer.data && !ibuf->byte_buffer.data)) {
    if (ibuf) {
      BKE_image_release_ibuf(ima, ibuf, lock);
    }
    BKE_report(op->reports, RPT_ERROR, "Could not acquire image buffer");
    return false;
  }

  const int tile_w = ibuf->x;
  const int tile_h = ibuf->y;
  const int tight_w = tight_max[0] - tight_min[0];
  const int tight_h = tight_max[1] - tight_min[1];
  const int margin_x = std::max(int(float(tight_w) * 0.5f), IMAGE_SELECT_WARP_MIN_MARGIN_PX);
  const int margin_y = std::max(int(float(tight_h) * 0.5f), IMAGE_SELECT_WARP_MIN_MARGIN_PX);

  const int cap_x0 = std::max(0, tight_min[0] - margin_x);
  const int cap_y0 = std::max(0, tight_min[1] - margin_y);
  const int cap_x1 = std::min(tile_w, tight_max[0] + margin_x);
  const int cap_y1 = std::min(tile_h, tight_max[1] + margin_y);
  const int cap_w = cap_x1 - cap_x0;
  const int cap_h = cap_y1 - cap_y0;

  if (cap_w <= 0 || cap_h <= 0) {
    BKE_image_release_ibuf(ima, ibuf, lock);
    BKE_report(op->reports, RPT_ERROR, "Selection bounding box is out of image bounds");
    return false;
  }

  const bool is_float = ibuf->float_buffer.data != nullptr;
  ImBuf *frag_ibuf = IMB_allocImBuf(
      cap_w, cap_h, is_float ? ImBufFlags::FloatData : ImBufFlags::ByteData);
  if (!frag_ibuf) {
    BKE_image_release_ibuf(ima, ibuf, lock);
    BKE_report(op->reports, RPT_ERROR, "Cannot allocate fragment buffer");
    return false;
  }
  if (is_float) {
    frag_ibuf->channels = ibuf->channels ? ibuf->channels : 4;
  }

  IMB_copy_rect(frag_ibuf, ibuf, int2{cap_x0, cap_y0}, int2{0, 0}, int2{cap_w, cap_h});

  if (is_float) {
    const ColorSpace *cs = ibuf->float_buffer.colorspace;
    if (cs) {
      IMB_colormanagement_assign_float_colorspace(frag_ibuf,
                                                   IMB_colormanagement_colorspace_get_name(cs));
    }
  }
  else {
    const ColorSpace *cs = ibuf->byte_buffer.colorspace;
    if (cs) {
      IMB_colormanagement_assign_byte_colorspace(frag_ibuf,
                                                  IMB_colormanagement_colorspace_get_name(cs));
    }
  }

  BKE_image_release_ibuf(ima, ibuf, lock);

  /* The mask is only sampled below, so take the `const Image *` overload; the mutable one would
   * advance the mask revision and invalidate the cached selection outline. */
  const ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(const_cast<const Image *>(ima),
                                                                 tile_number);
  /* The capture rect is clamped against the tile buffer, so the mask may only be indexed with it
   * when the two share a resolution. */
  if (!image_select_mask_matches(tile_mask, tile_w, tile_h)) {
    tile_mask = nullptr;
  }
  ImBuf *frag_mask = IMB_allocImBuf(cap_w, cap_h, ImBufFlags::Zero);
  IMB_alloc_float_pixels(frag_mask, 1);
  const float *src_mask = tile_mask ? tile_mask->float_data() : nullptr;
  float *dst_mask = frag_mask->float_data_for_write();
  if (!src_mask) {
    memset(dst_mask, 0, sizeof(float) * cap_w * cap_h);
  }
  else {
    for (int ly = 0; ly < cap_h; ly++) {
      for (int lx = 0; lx < cap_w; lx++) {
        dst_mask[ly * cap_w + lx] = src_mask[(cap_y0 + ly) * tile_mask->x + (cap_x0 + lx)];
      }
    }
  }

  SelectionTileFragment frag;
  frag.pixels.fragment_ibuf = frag_ibuf;
  frag.pixels.fragment_mask_ibuf = frag_mask;
  frag.edge_policy = BKE_image_paint_selection_edge_policy_get(ima);
  image_select_fragment_update_preview_buffers(frag);
  frag.geom.origin_px = int2{cap_x0, cap_y0};
  frag.geom.size_px = int2{cap_w, cap_h};
  frag.geom.selection_origin_px = int2{tight_min[0] - cap_x0, tight_min[1] - cap_y0};
  frag.geom.selection_size_px = int2{tight_w, tight_h};
  frag.geom.tile_size_px = int2{tile_w, tile_h};
  frag.geom.tile_number = tile_number;

  *r_fragment = std::move(frag);
  return true;
}

/**
 * UV rect of a warp fragment's capture area in global (UDIM) UV space: the bottom-left corner and
 * the size. Grid points are stored normalized to this rect, so screen mapping and the inverse
 * mapping in the drag modal both go through it.
 */
static void image_select_warp_capture_uv_rect(const SelectionTileFragment &frag,
                                              float2 *r_origin_uv,
                                              float2 *r_size_uv)
{
  const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
  const float tile_w = float(frag.geom.tile_size_px.x);
  const float tile_h = float(frag.geom.tile_size_px.y);
  *r_origin_uv = tile_uv + float2(float(frag.geom.origin_px.x) / tile_w,
                                  float(frag.geom.origin_px.y) / tile_h);
  *r_size_uv = float2(float(frag.geom.size_px.x) / tile_w, float(frag.geom.size_px.y) / tile_h);
}

void image_select_warp_init_grid(ImageSelectWarpState *state)
{
  const int grid_size = state->grid_size;
  const int point_count = grid_size * grid_size;
  state->src_pts.reinitialize(point_count);
  state->tgt_pts.reinitialize(point_count);

  const float2 cap_size = float2(float(state->fragment.geom.size_px.x),
                                 float(state->fragment.geom.size_px.y));
  const float2 sel_origin = float2(float(state->fragment.geom.selection_origin_px.x),
                                   float(state->fragment.geom.selection_origin_px.y));
  const float2 sel_size = float2(float(state->fragment.geom.selection_size_px.x),
                                 float(state->fragment.geom.selection_size_px.y));

  for (int y = 0; y < grid_size; y++) {
    for (int x = 0; x < grid_size; x++) {
      const float nx = float(x) / float(grid_size - 1);
      const float ny = float(y) / float(grid_size - 1);
      const float2 px = sel_origin + float2(nx, ny) * sel_size;
      const float2 uv = px / cap_size;
      const int idx = y * grid_size + x;
      state->src_pts[idx] = uv;
      state->tgt_pts[idx] = uv;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU preview
 * \{ */

constexpr int IMAGE_SELECT_WARP_CELL_SUBDIV = 8;

/** Evaluate a point on the whole grid_size^2 control grid at continuous grid coordinates
 * (gx, gy) in [0, grid_size-1] x [0, grid_size-1]. LINEAR uses the containing cell's bilinear
 * patch; SMOOTH uses the Catmull-Rom spline (see warp_grid_eval_smooth). */
static float2 warp_grid_eval(
    const float2 *pts, int grid_size, float gx, float gy, eImagePaint_WarpInterpolation interp)
{
  if (interp == IMAGE_PAINT_WARP_INTERP_SMOOTH) {
    return warp_grid_eval_smooth(pts, grid_size, gx, gy);
  }
  gx = std::clamp(gx, 0.0f, float(grid_size - 1));
  gy = std::clamp(gy, 0.0f, float(grid_size - 1));
  /* Clamp the cell (not the coordinate) so the local parameter reaches exactly 1.0 at the far
   * edge -- this reproduces the previous corner-based commit evaluation bit-for-bit. */
  const int cell_x = std::clamp(int(gx), 0, grid_size - 2);
  const int cell_y = std::clamp(int(gy), 0, grid_size - 2);
  const float u = gx - float(cell_x);
  const float v = gy - float(cell_y);
  float2 p00, p10, p01, p11;
  warp_grid_cell_corners(pts, grid_size, cell_x, cell_y, &p00, &p10, &p01, &p11);
  return warp_bilinear_eval(p00, p10, p01, p11, u, v);
}

/**
 * Resample both the undeformed src_pts grid and the user's deformed tgt_pts grid to
 * \a new_grid_size, preserving the current warp shape: each new grid point is evaluated on the
 * *old* grid's bilinear surface (the same evaluation tessellation/GPU preview and CPU commit
 * already use) at the matching continuous grid coordinate. Resampling src_pts this way reproduces
 * the same evenly-spaced grid init_grid() would build directly, since bilinear interpolation of
 * an axis-aligned evenly-spaced grid is linear.
 */
static void image_select_warp_resize_grid(ImageSelectWarpState *state, const int new_grid_size)
{
  const int old_grid_size = state->grid_size;
  if (new_grid_size == old_grid_size) {
    return;
  }

  Array<float2> new_src(new_grid_size * new_grid_size);
  Array<float2> new_tgt(new_grid_size * new_grid_size);
  for (int y = 0; y < new_grid_size; y++) {
    const float gy = float(y) / float(new_grid_size - 1) * float(old_grid_size - 1);
    for (int x = 0; x < new_grid_size; x++) {
      const float gx = float(x) / float(new_grid_size - 1) * float(old_grid_size - 1);
      const int idx = y * new_grid_size + x;
      new_src[idx] = warp_grid_eval(state->src_pts.data(), old_grid_size, gx, gy, state->interp);
      new_tgt[idx] = warp_grid_eval(state->tgt_pts.data(), old_grid_size, gx, gy, state->interp);
    }
  }

  state->grid_size = new_grid_size;
  state->src_pts = std::move(new_src);
  state->tgt_pts = std::move(new_tgt);
  /* Old snapshots are sized for the old grid_size and would corrupt tgt_pts if restored. */
  state->drag_position_history.clear();
  state->drag_point_idx = -1;
  state->hover_point_idx = -1;
}

/** Re-read ImagePaintSettings::warp_grid_size and resize the grid if it changed, marking the
 * currently applied revision up to date either way. Only called while not dragging (dragging
 * tracks a specific point index that a resize would invalidate). */
static void image_select_warp_apply_settings_revision(const bContext *C,
                                                      ImageSelectWarpState *state)
{
  state->applied_settings_revision = image_paint_warp_settings_revision;
  const Scene *scene = CTX_data_scene(C);
  state->interp = eImagePaint_WarpInterpolation(scene->toolsettings->imapaint.warp_interpolation);
  const int new_grid_size = std::clamp(int(scene->toolsettings->imapaint.warp_grid_size),
                                       IMAGE_SELECT_WARP_GRID_MIN,
                                       IMAGE_SELECT_WARP_GRID_MAX);
  image_select_warp_resize_grid(state, new_grid_size);
}

/**
 * Region post-pixel draw callback: renders the warped fragment preview (dense tessellation of
 * the current tgt_pts grid, sampling fragment_ibuf via src_pts texcoords), the control-grid
 * wireframe, and the grid_size^2 control-point handles. Registered in invoke, removed on exit.
 */
static void draw_select_warp_preview(const bContext *C, ARegion *region, void *arg)
{
  ImageSelectWarpState *state = static_cast<ImageSelectWarpState *>(arg);
  if (!state->fragment.pixels.fragment_ibuf) {
    return;
  }

  /* Apply a pending "Grid Size" tool-setting change here rather than only in the paint-cursor
   * hover detector: this callback runs on every actual redraw of the region regardless of mouse
   * position (the RNA update callback forces one via NC_SPACE | ND_SPACE_IMAGE), whereas the
   * paint cursor only runs while the mouse hovers this region. Skipped while dragging, same as
   * the hover detector. */
  if (!state->is_dragging &&
      state->applied_settings_revision != image_paint_warp_settings_revision)
  {
    image_select_warp_apply_settings_revision(C, state);
  }

  const int grid_size = state->grid_size;
  const int cells = grid_size - 1;
  const int tess_res = cells * IMAGE_SELECT_WARP_CELL_SUBDIV + 1;

  float2 cap_origin_uv, cap_size_uv;
  image_select_warp_capture_uv_rect(state->fragment, &cap_origin_uv, &cap_size_uv);

  auto to_screen = [&](const float2 &capture_normalized) -> float2 {
    const float2 uv = cap_origin_uv + capture_normalized * cap_size_uv;
    float sx, sy;
    ui::view2d_view_to_region_fl(&region->v2d, uv.x, uv.y, &sx, &sy);
    return float2(sx, sy);
  };

  /* Dense tessellation of the deformed grid, textured with the undeformed grid's UVs. */
  Vector<float2> screen_verts;
  Vector<float2> tex_coords;
  screen_verts.reserve(tess_res * tess_res);
  tex_coords.reserve(tess_res * tess_res);
  for (int y = 0; y < tess_res; y++) {
    const float gy = float(y) / float(tess_res - 1) * float(cells);
    for (int x = 0; x < tess_res; x++) {
      const float gx = float(x) / float(tess_res - 1) * float(cells);
      screen_verts.append(
          to_screen(warp_grid_eval(state->tgt_pts.data(), grid_size, gx, gy, state->interp)));
      tex_coords.append(warp_grid_eval(state->src_pts.data(), grid_size, gx, gy, state->interp));
    }
  }

  Vector<uint3> tris;
  tris.reserve((tess_res - 1) * (tess_res - 1) * 2);
  for (int y = 0; y < tess_res - 1; y++) {
    for (int x = 0; x < tess_res - 1; x++) {
      const uint i0 = uint(y * tess_res + x);
      const uint i1 = i0 + 1;
      const uint i2 = i0 + uint(tess_res);
      const uint i3 = i2 + 1;
      tris.append(uint3(i0, i1, i2));
      tris.append(uint3(i1, i3, i2));
    }
  }

  /* Create and bind GPU texture from fragment_ibuf. */
  const ImBuf *frag = state->fragment.pixels.fragment_ibuf;
  gpu::Texture *tex = nullptr;
  if (frag->float_buffer.data) {
    const int ch = frag->channels ? frag->channels : 4;
    const gpu::TextureFormat fmt = (ch >= 4) ? gpu::TextureFormat::SFLOAT_16_16_16_16 :
                                   (ch == 3) ? gpu::TextureFormat::SFLOAT_16_16_16 :
                                               gpu::TextureFormat::SFLOAT_16;
    tex = GPU_texture_create_2d(
        "warp_preview", frag->x, frag->y, 1, fmt, GPU_TEXTURE_USAGE_SHADER_READ, nullptr);
    GPU_texture_update(tex, GPU_DATA_FLOAT, frag->float_buffer.data);
  }
  else if (frag->byte_buffer.data) {
    tex = GPU_texture_create_2d("warp_preview",
                                frag->x,
                                frag->y,
                                1,
                                gpu::TextureFormat::UNORM_8_8_8_8,
                                GPU_TEXTURE_USAGE_SHADER_READ,
                                nullptr);
    GPU_texture_update(tex, GPU_DATA_UBYTE, frag->byte_buffer.data);
  }

  if (tex) {
    GPU_texture_filter_mode(tex, false);
    GPU_texture_extend_mode(tex, GPU_SAMPLER_EXTEND_MODE_EXTEND);
    GPU_texture_bind(tex, 0);

    GPU_blend(GPU_BLEND_ALPHA);
    GPUVertFormat *format = immVertexFormat();
    const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
    const uint texco = GPU_vertformat_attr_add(
        format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_IMAGE);
    immBindTexture("image", tex);

    immBegin(GPU_PRIM_TRIS, int(tris.size()) * 3);
    for (const uint3 &tri : tris) {
      for (const uint idx : {tri.x, tri.y, tri.z}) {
        immAttr2f(texco, tex_coords[idx].x, tex_coords[idx].y);
        immVertex2f(pos, screen_verts[idx].x, screen_verts[idx].y);
      }
    }
    immEnd();
    immUnbindProgram();
    GPU_texture_unbind(tex);
    GPU_texture_free(tex);
    GPU_blend(GPU_BLEND_NONE);
  }

  /* Control-grid wireframe. */
  GPU_line_smooth(true);
  const uint line_pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.5f);
  immBegin(GPU_PRIM_LINES, grid_size * (grid_size - 1) * 4);
  for (int y = 0; y < grid_size; y++) {
    for (int x = 0; x < grid_size; x++) {
      const int idx = y * grid_size + x;
      const float2 p_curr = to_screen(state->tgt_pts[idx]);
      if (x < grid_size - 1) {
        const float2 p_next = to_screen(state->tgt_pts[idx + 1]);
        immVertex2f(line_pos, p_curr.x, p_curr.y);
        immVertex2f(line_pos, p_next.x, p_next.y);
      }
      if (y < grid_size - 1) {
        const float2 p_next = to_screen(state->tgt_pts[idx + grid_size]);
        immVertex2f(line_pos, p_curr.x, p_curr.y);
        immVertex2f(line_pos, p_next.x, p_next.y);
      }
    }
  }
  immEnd();
  immUnbindProgram();
  GPU_line_smooth(false);

  /* Control-point handles. Drag (red, largest) takes priority over hover (white, enlarged) over
   * the default idle marker (yellow). */
  const float h_size = 4.0f;
  for (int i = 0; i < state->tgt_pts.size(); i++) {
    const float2 p = to_screen(state->tgt_pts[i]);
    const bool is_drag = (i == state->drag_point_idx);
    const bool is_hover = (!state->is_dragging && i == state->hover_point_idx);
    const float s = is_drag ? h_size * 1.5f : is_hover ? h_size * 1.25f : h_size;
    const uint pt_pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    if (is_drag) {
      immUniformColor4f(1.0f, 0.2f, 0.2f, 1.0f);
    }
    else if (is_hover) {
      immUniformColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    }
    else {
      immUniformColor4f(1.0f, 0.85f, 0.0f, 0.9f);
    }
    immBegin(GPU_PRIM_TRI_FAN, 4);
    immVertex2f(pt_pos, p.x - s, p.y - s);
    immVertex2f(pt_pos, p.x + s, p.y - s);
    immVertex2f(pt_pos, p.x + s, p.y + s);
    immVertex2f(pt_pos, p.x - s, p.y + s);
    immEnd();
    immUnbindProgram();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Commit
 * \{ */

/**
 * A single forward-mapped triangle of the commit tessellation: \a d0..d2 are its *deformed*
 * (destination) corners and \a s0..s2 the corresponding *undeformed* (source) corners, both in
 * continuous capture-pixel space (same convention as draw_select_warp_preview()'s screen-space
 * tessellation, just without the view2d step). \a bbox_min/bbox_max is the destination-space
 * axis-aligned bounding box, for a cheap reject test before the exact point-in-triangle check.
 */
struct WarpTriangle {
  float2 d0, d1, d2;
  float2 s0, s1, s2;
  float2 bbox_min, bbox_max;
};

/**
 * Barycentric weights of \a p in triangle (a, b, c); returns false when \a p is outside the
 * triangle (beyond a small tolerance) or the triangle is degenerate. Standard dot-product
 * formulation (Ericson, "Real-Time Collision Detection") -- unlike solving the cell's bilinear
 * patch as one quadratic (the previous approach), this cannot fail to find a point that is
 * genuinely inside a non-degenerate triangle, so it cannot leave gaps at cell/triangle seams.
 */
static bool warp_barycentric(const float2 &a,
                             const float2 &b,
                             const float2 &c,
                             const float2 &p,
                             float *r_u,
                             float *r_v,
                             float *r_w)
{
  const float2 v0 = b - a;
  const float2 v1 = c - a;
  const float2 v2 = p - a;
  const float d00 = v0.x * v0.x + v0.y * v0.y;
  const float d01 = v0.x * v1.x + v0.y * v1.y;
  const float d11 = v1.x * v1.x + v1.y * v1.y;
  const float d20 = v2.x * v0.x + v2.y * v0.y;
  const float d21 = v2.x * v1.x + v2.y * v1.y;
  const float denom = d00 * d11 - d01 * d01;
  if (std::abs(denom) < 1e-10f) {
    return false;
  }
  const float v = (d11 * d20 - d01 * d21) / denom;
  const float w = (d00 * d21 - d01 * d20) / denom;
  const float u = 1.0f - v - w;
  constexpr float epsilon = 1e-4f;
  if (u < -epsilon || v < -epsilon || w < -epsilon) {
    return false;
  }
  *r_u = u;
  *r_v = v;
  *r_w = w;
  return true;
}

static void warp_append_triangle(Vector<WarpTriangle> &tris,
                                 const float2 &d0,
                                 const float2 &d1,
                                 const float2 &d2,
                                 const float2 &s0,
                                 const float2 &s1,
                                 const float2 &s2)
{
  WarpTriangle tri;
  tri.d0 = d0;
  tri.d1 = d1;
  tri.d2 = d2;
  tri.s0 = s0;
  tri.s1 = s1;
  tri.s2 = s2;
  tri.bbox_min = float2(std::min({d0.x, d1.x, d2.x}), std::min({d0.y, d1.y, d2.y}));
  tri.bbox_max = float2(std::max({d0.x, d1.x, d2.x}), std::max({d0.y, d1.y, d2.y}));
  tris.append(tri);
}

/**
 * Build the forward commit tessellation: every cell of the deformed grid subdivided into
 * IMAGE_SELECT_WARP_CELL_SUBDIV^2 sub-quads (2 triangles each), matching
 * draw_select_warp_preview()'s tessellation exactly so the committed result reproduces what the
 * live preview showed, with no gaps at cell boundaries. Built once per commit (shared read-only
 * across the parallel row tasks), not per destination pixel.
 */
static void warp_build_commit_triangles(const ImageSelectWarpState &state,
                                        Vector<WarpTriangle> &r_tris)
{
  const int grid_size = state.grid_size;
  const int cells = grid_size - 1;
  const int subdiv = IMAGE_SELECT_WARP_CELL_SUBDIV;
  const eImagePaint_WarpInterpolation interp = state.interp;
  const float2 cap_size = float2(float(state.fragment.geom.size_px.x),
                                 float(state.fragment.geom.size_px.y));
  const float2 *tgt = state.tgt_pts.data();
  const float2 *src = state.src_pts.data();

  r_tris.reserve(int64_t(cells) * cells * subdiv * subdiv * 2);

  /* Sample the deformed (tgt) and undeformed (src) grids at the same continuous grid coordinates,
   * subdivided IMAGE_SELECT_WARP_CELL_SUBDIV times per cell -- identical sampling to
   * draw_select_warp_preview(), so the committed result reproduces the live preview with no gaps.
   * Catmull-Rom needs the full grid (its 4x4 neighborhood), so evaluation goes through
   * warp_grid_eval per sample rather than precomputing one cell's 4 corners; in LINEAR mode this
   * yields bit-identical values to the previous corner-based bilinear sub-sampling. */
  for (int cy = 0; cy < cells; cy++) {
    for (int cx = 0; cx < cells; cx++) {
      for (int sy = 0; sy < subdiv; sy++) {
        const float gy0 = float(cy) + float(sy) / float(subdiv);
        const float gy1 = float(cy) + float(sy + 1) / float(subdiv);
        for (int sx = 0; sx < subdiv; sx++) {
          const float gx0 = float(cx) + float(sx) / float(subdiv);
          const float gx1 = float(cx) + float(sx + 1) / float(subdiv);

          const float2 d00 = warp_grid_eval(tgt, grid_size, gx0, gy0, interp) * cap_size;
          const float2 d10 = warp_grid_eval(tgt, grid_size, gx1, gy0, interp) * cap_size;
          const float2 d01 = warp_grid_eval(tgt, grid_size, gx0, gy1, interp) * cap_size;
          const float2 d11 = warp_grid_eval(tgt, grid_size, gx1, gy1, interp) * cap_size;

          const float2 s00 = warp_grid_eval(src, grid_size, gx0, gy0, interp) * cap_size;
          const float2 s10 = warp_grid_eval(src, grid_size, gx1, gy0, interp) * cap_size;
          const float2 s01 = warp_grid_eval(src, grid_size, gx0, gy1, interp) * cap_size;
          const float2 s11 = warp_grid_eval(src, grid_size, gx1, gy1, interp) * cap_size;

          warp_append_triangle(r_tris, d00, d10, d01, s00, s10, s01);
          warp_append_triangle(r_tris, d10, d11, d01, s10, s11, s01);
        }
      }
    }
  }
}

struct WarpTriangleHit {
  bool found = false;
  /* Source position in continuous capture-pixel space (same convention as \a WarpTriangle). */
  float2 src_px = {0.0f, 0.0f};
};

/**
 * Uniform bin grid over the destination-space bounding boxes of the commit tessellation, so a
 * pixel only tests the triangles that can possibly contain it.
 *
 * A direct cell index is not usable here: the lookup happens in *deformed* space, which is only a
 * regular grid before the warp. Binning the deformed bounding boxes stays correct for arbitrary
 * (including folded) deformations.
 *
 * Stored CSR-style: bin `b` owns `tri_indices[offsets[b] .. offsets[b + 1])`, in ascending
 * triangle index.
 */
struct WarpTriangleGrid {
  int res_x = 0;
  int res_y = 0;
  float2 origin = {0.0f, 0.0f};
  /* Reciprocal bin size; multiplying avoids a divide per lookup. */
  float2 inv_bin_size = {0.0f, 0.0f};
  Vector<int> offsets;
  Vector<int> tri_indices;
};

/** Bin coordinate of \a p, clamped so points outside the built extent land in the border bin. */
static int2 warp_triangle_grid_bin_of(const WarpTriangleGrid &grid, const float2 &p)
{
  const int bx = int(std::floor((p.x - grid.origin.x) * grid.inv_bin_size.x));
  const int by = int(std::floor((p.y - grid.origin.y) * grid.inv_bin_size.y));
  return int2(std::clamp(bx, 0, grid.res_x - 1), std::clamp(by, 0, grid.res_y - 1));
}

/**
 * Build the bin grid for \a tris. Aims at roughly one triangle per bin, clamped so a degenerate
 * or tiny tessellation cannot blow up the allocation.
 */
static void warp_build_triangle_grid(const Vector<WarpTriangle> &tris,
                                     const int2 cap_size,
                                     WarpTriangleGrid &r_grid)
{
  const int tri_num = int(tris.size());
  if (tri_num == 0 || cap_size.x <= 0 || cap_size.y <= 0) {
    return;
  }

  /* One bin per triangle keeps both the build cost and the per-pixel candidate list at O(1)
   * amortized, since the tessellation is dense and evenly distributed over the capture area. */
  const int res = std::clamp(int(std::sqrt(double(tri_num))), 1, 256);
  r_grid.res_x = res;
  r_grid.res_y = res;
  r_grid.origin = float2(0.0f, 0.0f);
  r_grid.inv_bin_size = float2(float(res) / float(cap_size.x), float(res) / float(cap_size.y));

  const int bin_num = res * res;

  /* Counting sort: one pass to size each bin, one to fill it. Filling in ascending triangle order
   * is what preserves the linear scan's tie-break (see warp_find_triangle). */
  r_grid.offsets.resize(bin_num + 1, 0);
  for (int i = 0; i < tri_num; i++) {
    const int2 lo = warp_triangle_grid_bin_of(r_grid, tris[i].bbox_min);
    const int2 hi = warp_triangle_grid_bin_of(r_grid, tris[i].bbox_max);
    for (int by = lo.y; by <= hi.y; by++) {
      for (int bx = lo.x; bx <= hi.x; bx++) {
        r_grid.offsets[by * res + bx + 1]++;
      }
    }
  }
  for (int b = 0; b < bin_num; b++) {
    r_grid.offsets[b + 1] += r_grid.offsets[b];
  }

  Vector<int> cursor(r_grid.offsets.as_span().drop_back(1));
  r_grid.tri_indices.resize(r_grid.offsets[bin_num]);
  for (int i = 0; i < tri_num; i++) {
    const int2 lo = warp_triangle_grid_bin_of(r_grid, tris[i].bbox_min);
    const int2 hi = warp_triangle_grid_bin_of(r_grid, tris[i].bbox_max);
    for (int by = lo.y; by <= hi.y; by++) {
      for (int bx = lo.x; bx <= hi.x; bx++) {
        r_grid.tri_indices[cursor[by * res + bx]++] = i;
      }
    }
  }
}

/**
 * Find the triangle of the commit tessellation containing \a p, in destination space.
 *
 * Equivalent to a linear scan over \a tris, including which triangle wins where the deformation
 * folds and several overlap: a triangle is registered in every bin its bounding box touches, and
 * bin coordinates are monotonic in position, so the bin holding \a p is guaranteed to list every
 * triangle whose bbox contains \a p -- in ascending index, exactly the order the scan visited
 * them. The bin is therefore a superset of the accepted candidates and the first acceptance is
 * the same one.
 */
static WarpTriangleHit warp_find_triangle(const Vector<WarpTriangle> &tris,
                                          const WarpTriangleGrid &grid,
                                          const float2 &p)
{
  WarpTriangleHit hit;
  if (grid.res_x == 0) {
    return hit;
  }
  const int2 bin = warp_triangle_grid_bin_of(grid, p);
  const int b = bin.y * grid.res_x + bin.x;
  for (int slot = grid.offsets[b]; slot < grid.offsets[b + 1]; slot++) {
    const WarpTriangle &tri = tris[grid.tri_indices[slot]];
    if (p.x < tri.bbox_min.x || p.x > tri.bbox_max.x || p.y < tri.bbox_min.y ||
        p.y > tri.bbox_max.y)
    {
      continue;
    }
    float u, v, w;
    if (warp_barycentric(tri.d0, tri.d1, tri.d2, p, &u, &v, &w)) {
      hit.found = true;
      hit.src_px = tri.s0 * u + tri.s1 * v + tri.s2 * w;
      return hit;
    }
  }
  return hit;
}

struct WarpCommitTaskData {
  const ImageSelectWarpState *state = nullptr;
  const Vector<WarpTriangle> *triangles = nullptr;
  const WarpTriangleGrid *triangle_grid = nullptr;
  ImBuf *dst_ibuf = nullptr;
  ImBuf *dst_mask_ibuf = nullptr;
  int2 dst_origin_px = {0, 0};

  /* Write pointers, resolved once in image_select_warp_commit() before the parallel dispatch --
   * see the comment there for why resolving them per-row (from worker threads) is unsafe. */
  int dst_channels = 4;
  float *dst_float = nullptr;
  uint8_t *dst_byte = nullptr;
  float *dst_mask = nullptr;
};

static void image_select_warp_commit_row_task(void *__restrict userdata,
                                              const int y,
                                              const TaskParallelTLS *__restrict /*tls*/)
{
  WarpCommitTaskData &data = *static_cast<WarpCommitTaskData *>(userdata);
  const ImageSelectWarpState &state = *data.state;
  const int cap_w = state.fragment.geom.size_px.x;
  const int cap_h = state.fragment.geom.size_px.y;
  const ImBuf *frag_ibuf = state.fragment.pixels.fragment_ibuf;
  const bool is_float = frag_ibuf->float_data() != nullptr;
  const int channels = frag_ibuf->channels ? frag_ibuf->channels : 4;

  const int dst_y = data.dst_origin_px.y + y;
  if (dst_y < 0 || dst_y >= data.dst_ibuf->y) {
    return;
  }

  const int dst_channels = data.dst_channels;
  float *dst_float = data.dst_float;
  uint8_t *dst_byte = data.dst_byte;
  float *dst_mask = data.dst_mask;

  for (int x = 0; x < cap_w; x++) {
    const int dst_x = data.dst_origin_px.x + x;
    if (dst_x < 0 || dst_x >= data.dst_ibuf->x) {
      continue;
    }

    /* Default to an identity "restore" sample -- this destination pixel's own position in the
     * captured fragment, unwarped. Overridden below when the forward tessellation actually
     * covers it with genuinely selected source content. */
    float src_fx = float(x);
    float src_fy = float(y);
    float mask_weight = 0.0f;
    bool is_restore = true;

    const float2 p_px = float2(float(x) + 0.5f, float(y) + 0.5f);
    const WarpTriangleHit hit = warp_find_triangle(*data.triangles, *data.triangle_grid, p_px);
    if (hit.found) {
      const float warp_src_fx = std::clamp(hit.src_px.x - 0.5f, 0.0f, float(cap_w) - 1.0001f);
      const float warp_src_fy = std::clamp(hit.src_px.y - 0.5f, 0.0f, float(cap_h) - 1.0001f);

      float warp_mask_weight = 1.0f;
      if (state.fragment.edge_policy.use_outward_feather &&
          state.fragment.preview.fragment_blend_mask_ibuf)
      {
        warp_mask_weight = image_select_sample_mask_bilinear(
            state.fragment.preview.fragment_blend_mask_ibuf, warp_src_fx, warp_src_fy);
      }
      else if (state.fragment.pixels.fragment_mask_ibuf) {
        const float *mdata = state.fragment.pixels.fragment_mask_ibuf->float_data();
        const int mx = int(std::round(warp_src_fx));
        const int my = int(std::round(warp_src_fy));
        warp_mask_weight =
            mdata[std::clamp(my, 0, cap_h - 1) * cap_w + std::clamp(mx, 0, cap_w - 1)] >
                    SELECTION_MASK_THRESHOLD ?
                1.0f :
                0.0f;
      }
      /* Deliberately not #SELECTION_MASK_THRESHOLD: this only asks whether the warped content
       * covers this pixel at all, so any non-negligible weight counts. The feathered edge is
       * meant to composite its color at partial strength while staying outside the binary
       * selection, which the mask write below reconstructs at #SELECTION_MASK_THRESHOLD. In the
       * non-feathered path above `warp_mask_weight` is already 0 or 1, so both agree there. */
      if (warp_mask_weight > 0.001f) {
        src_fx = warp_src_fx;
        src_fy = warp_src_fy;
        mask_weight = warp_mask_weight;
        is_restore = false;
      }
    }

    /* The selection mask deforms together with the content -- it follows mask_weight (sampled at
     * the warped source), same as the color. A pixel the deformed shape no longer covers reverts
     * to background and is deselected. Written unconditionally (before the color path's early
     * `continue`s below) so it never keeps a stale pre-commit value. */
    if (dst_mask) {
      dst_mask[dst_y * data.dst_mask_ibuf->x + dst_x] = (!is_restore &&
                                                         mask_weight > SELECTION_MASK_THRESHOLD) ?
                                                             1.0f :
                                                             0.0f;
    }

    /* This destination pixel is not covered by any (selected) warped content -- either no
     * tessellated triangle contains it, or the source point it maps to was never part of the
     * selection. #image_select_fragment_lift_source() only clears pixels that WERE inside the
     * original (undeformed) selection mask at their own position, so restore exactly those here
     * to avoid leaving a hole; anything else was never touched by lift and can be skipped.
     * (Unlike Move, Warp's capture area sits at the same canvas location as the destination, so
     * there is no separate "old" location where a hole would be expected.) */
    if (is_restore) {
      const ImBuf *orig_mask_ibuf = state.fragment.pixels.fragment_mask_ibuf;
      const bool was_selected = orig_mask_ibuf && orig_mask_ibuf->float_data()[y * cap_w + x] >
                                                      SELECTION_MASK_THRESHOLD;
      if (!was_selected) {
        continue;
      }
    }

    const int bx0 = int(src_fx);
    const int by0 = int(src_fy);
    const int bx1 = std::min(cap_w - 1, bx0 + 1);
    const int by1 = std::min(cap_h - 1, by0 + 1);
    const float wx1 = src_fx - float(bx0);
    const float wx0 = 1.0f - wx1;
    const float wy1 = src_fy - float(by0);
    const float wy0 = 1.0f - wy1;

    float frag_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (is_float) {
      const float *src = frag_ibuf->float_data();
      for (int c = 0; c < channels; c++) {
        frag_color[c] = src[(by0 * cap_w + bx0) * channels + c] * wx0 * wy0 +
                        src[(by0 * cap_w + bx1) * channels + c] * wx1 * wy0 +
                        src[(by1 * cap_w + bx0) * channels + c] * wx0 * wy1 +
                        src[(by1 * cap_w + bx1) * channels + c] * wx1 * wy1;
      }
    }
    else {
      const uint8_t *src = frag_ibuf->byte_data();
      for (int c = 0; c < 4; c++) {
        frag_color[c] = (float(src[(by0 * cap_w + bx0) * 4 + c]) * wx0 * wy0 +
                         float(src[(by0 * cap_w + bx1) * 4 + c]) * wx1 * wy0 +
                         float(src[(by1 * cap_w + bx0) * 4 + c]) * wx0 * wy1 +
                         float(src[(by1 * cap_w + bx1) * 4 + c]) * wx1 * wy1) /
                        255.0f;
      }
    }
    const float frag_alpha = (is_float && channels < 4) ? 1.0f : frag_color[3];
    /* Restored pixels are an exact copy-back of the lifted content (dst was cleared to zero by
     * lift for exactly these pixels), not a fresh composite over whatever else is at dst -- so
     * force a full overwrite regardless of the fragment's own alpha. */
    const float blend = is_restore ? 1.0f : (mask_weight * frag_alpha);
    if (blend <= 0.001f) {
      continue;
    }

    if (dst_float) {
      float *dst_px = dst_float + (size_t(dst_y) * data.dst_ibuf->x + dst_x) * dst_channels;
      for (int c = 0; c < dst_channels; c++) {
        dst_px[c] = (1.0f - blend) * dst_px[c] + blend * frag_color[c];
      }
    }
    else if (dst_byte) {
      uint8_t *dst_px = dst_byte + (size_t(dst_y) * data.dst_ibuf->x + dst_x) * 4;
      for (int c = 0; c < 4; c++) {
        dst_px[c] = uint8_t(std::clamp(
            (1.0f - blend) * float(dst_px[c]) + blend * frag_color[c] * 255.0f, 0.0f, 255.0f));
      }
    }
  }
}

void image_select_warp_commit(bContext *C, ImageSelectWarpState *state, ReportList *reports)
{
  if (!state) {
    return;
  }
  SpaceImage *sima = state->owner_sima ? state->owner_sima : CTX_wm_space_image(C);
  if (!sima || !sima->image || !state->fragment.pixels.fragment_ibuf) {
    image_select_fragment_undo_push_end_if_open(state->undo_begun);
    return;
  }
  Image *ima = sima->image;

  ImageUser tile_iuser = state->iuser;
  tile_iuser.tile = state->fragment.geom.tile_number;
  void *lock;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &tile_iuser, &lock);
  if (!ibuf || (!ibuf->float_buffer.data && !ibuf->byte_buffer.data)) {
    if (ibuf) {
      BKE_image_release_ibuf(ima, ibuf, lock);
    }
    image_select_fragment_undo_push_end_if_open(state->undo_begun);
    return;
  }
  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;

  /* Built once, here, and shared read-only across the parallel row tasks below -- see
   * warp_build_commit_triangles() for why the commit rasterizes forward (matching the GPU
   * preview's tessellation) rather than solving each cell's bilinear patch backward per pixel. */
  Vector<WarpTriangle> triangles;
  warp_build_commit_triangles(*state, triangles);

  /* Bin the tessellation once so each destination pixel only tests its own neighborhood instead
   * of every triangle; also shared read-only across the row tasks. */
  WarpTriangleGrid triangle_grid;
  warp_build_triangle_grid(triangles, state->fragment.geom.size_px, triangle_grid);

  WarpCommitTaskData task_data;
  task_data.state = state;
  task_data.triangles = &triangles;
  task_data.triangle_grid = &triangle_grid;
  task_data.dst_ibuf = ibuf;
  task_data.dst_mask_ibuf = BKE_image_paint_selection_mask_lookup(
      ima, state->fragment.geom.tile_number);
  /* The row task clamps its writes against `dst_ibuf`, so the mask may only be written through
   * when it shares that resolution. Dropping it leaves the pixels committed and the mask stale,
   * which is recoverable; writing it would run past the mask allocation. */
  if (!image_select_mask_matches(task_data.dst_mask_ibuf, ibuf)) {
    task_data.dst_mask_ibuf = nullptr;
    BKE_report(reports,
               RPT_WARNING,
               "Selection mask resolution does not match the image; pixels were warped but the "
               "selection was left unchanged");
  }
  task_data.dst_origin_px = state->fragment.geom.origin_px;

  /* Resolve write pointers once, here on the calling thread, before the parallel dispatch.
   * ImBuf::float_data_for_write()/byte_data_for_write() are not safe to call concurrently: they
   * lazily materialize the buffer via #ImplicitSharingPtr (ensuring single ownership before
   * returning a writable pointer), which involves refcount/allocation bookkeeping that is not
   * thread-safe. Calling them from inside image_select_warp_commit_row_task (executed per-row by
   * worker threads via BLI_task_parallel_range) raced on that bookkeeping and crashed in
   * ImplicitSharingPtr::remove_user_and_delete_if_last. Compare gradient's
   * GradientRowTaskData::canvas_float_data/canvas_byte_data, which are resolved the same way. */
  task_data.dst_channels = ibuf->channels ? ibuf->channels : 4;
  task_data.dst_float = ibuf->float_data_for_write();
  task_data.dst_byte = task_data.dst_float ? nullptr : ibuf->byte_data_for_write();
  task_data.dst_mask = task_data.dst_mask_ibuf ? task_data.dst_mask_ibuf->float_data_for_write() :
                                                 nullptr;

  TaskParallelSettings settings;
  BLI_parallel_range_settings_defaults(&settings);
  settings.min_iter_per_thread = 8;
  BLI_task_parallel_range(0,
                          state->fragment.geom.size_px.y,
                          &task_data,
                          image_select_warp_commit_row_task,
                          &settings);

  BKE_image_mark_dirty(ima, ibuf);
  rcti dirty;
  BLI_rcti_init(&dirty,
                state->fragment.geom.origin_px.x,
                state->fragment.geom.origin_px.x + state->fragment.geom.size_px.x,
                state->fragment.geom.origin_px.y,
                state->fragment.geom.origin_px.y + state->fragment.geom.size_px.y);
  ImageTile *tile = BKE_image_get_tile(ima, state->fragment.geom.tile_number);
  BKE_image_partial_update_mark_region(ima, tile, ibuf, &dirty);
  BKE_image_release_ibuf(ima, ibuf, lock);

  image_select_fragment_undo_push_end_if_open(state->undo_begun);
  BKE_image_free_gputextures(ima);
  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Confirm / Cancel / Undo operators
 * \{ */

/* Defined in the "Invoke / modal / poll" section below; forward-declared here because the
 * confirm/cancel/undo_step operators reference it as their poll. */
static bool image_select_warp_floating_poll(bContext *C);

void image_select_warp_session_end_for_takeover(bContext *C, SpaceImage *sima)
{
  if (!sima || !sima->runtime) {
    return;
  }
  ImageSelectWarpState *&state_ref = sima->runtime->paint_select.warp;
  if (!state_ref) {
    return;
  }
  ImageSelectWarpState *state = state_ref;
  /* Cleared before the commit, not after: #image_select_warp_commit notifies and tags the image,
   * and anything that looks the session up again in response must not find the state being torn
   * down here. */
  state_ref = nullptr;
  /* A control-point drag may still be in progress when another tool takes over (the modal handler
   * is left registered and exits on its next event, seeing a null state). Give the window its
   * normal cursor and the status bar its normal text back here. */
  image_select_floating_drag_end(C, state);
  image_select_floating_status_clear(C);
  /* No report list: the takeover is not the user's own confirm, so a mask-resolution warning has
   * no operator to surface it on. #BKE_report tolerates a null list and still logs. */
  image_select_warp_commit(C, state, nullptr);
  image_select_warp_state_free(state);
}

static wmOperatorStatus image_select_warp_confirm_exec(bContext *C, wmOperator *op)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectWarpState *&state_ref = sima->runtime->paint_select.warp;
  if (!state_ref) {
    return OPERATOR_CANCELLED;
  }
  image_select_warp_commit(C, state_ref, op->reports);
  image_select_warp_state_free(state_ref);
  state_ref = nullptr;
  ARegion *region = CTX_wm_region(C);
  if (region) {
    ED_region_tag_redraw(region);
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_warp_confirm(wmOperatorType *ot)
{
  ot->name = "Confirm Warp Selection";
  ot->idname = "PAINT_OT_image_select_warp_confirm";
  ot->description = "Apply the current warp deformation to the canvas";

  ot->exec = image_select_warp_confirm_exec;
  ot->poll = image_select_warp_floating_poll;

  /* No OPTYPE_UNDO: commit closes the image undo step opened while the fragment was lifted.
   * Letting WM finalize that step first would leave push_end with nothing to close.
   * See the note in #PAINT_OT_image_select_move. */
  ot->flag = OPTYPE_REGISTER;
}

static wmOperatorStatus image_select_warp_cancel_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectWarpState *&state_ref = sima->runtime->paint_select.warp;
  if (!state_ref) {
    return OPERATOR_CANCELLED;
  }
  image_select_fragment_restore_source(
      C, sima->image, state_ref->iuser, {state_ref->fragment}, state_ref->undo_begun);
  image_select_warp_state_free(state_ref);
  state_ref = nullptr;
  ARegion *region = CTX_wm_region(C);
  if (region) {
    ED_region_tag_redraw(region);
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_warp_cancel(wmOperatorType *ot)
{
  ot->name = "Cancel Warp Selection";
  ot->idname = "PAINT_OT_image_select_warp_cancel";
  ot->description = "Discard the current warp and restore the original fragment";

  ot->exec = image_select_warp_cancel_exec;
  ot->poll = image_select_warp_floating_poll;

  ot->flag = OPTYPE_REGISTER;
}

static wmOperatorStatus image_select_warp_undo_step_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectWarpState *&state_ref = sima->runtime->paint_select.warp;
  if (!state_ref) {
    return OPERATOR_CANCELLED;
  }
  if (state_ref->drag_position_history.is_empty()) {
    /* No history: cancel entirely. */
    image_select_fragment_restore_source(
        C, sima->image, state_ref->iuser, {state_ref->fragment}, state_ref->undo_begun);
    image_select_warp_state_free(state_ref);
    state_ref = nullptr;
  }
  else {
    state_ref->tgt_pts = state_ref->drag_position_history.last();
    state_ref->drag_position_history.remove_last();
  }
  ARegion *region = CTX_wm_region(C);
  if (region) {
    ED_region_tag_redraw(region);
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_warp_undo_step(wmOperatorType *ot)
{
  ot->name = "Warp Selection Undo Step";
  ot->idname = "PAINT_OT_image_select_warp_undo_step";
  ot->description = "Undo the last drag gesture (or restore if no history)";

  ot->exec = image_select_warp_undo_step_exec;
  ot->poll = image_select_warp_floating_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invoke / modal / poll
 * \{ */

bool image_select_warp_is_floating(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  return image_select_warp_is_floating_in_space(sima);
}

bool image_select_warp_is_floating_in_space(const SpaceImage *sima)
{
  if (!sima || !sima->runtime) {
    return false;
  }
  return image_select_floating_state_owns(sima->runtime->paint_select.warp, sima);
}

static bool image_select_warp_floating_poll(bContext *C)
{
  return image_select_warp_is_floating(C);
}

static bool image_select_warp_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return false;
  }
  if (image_select_floating_state_owns(sima->runtime->paint_select.warp, sima)) {
    return true;
  }
  if (image_select_transform_is_floating(C)) {
    return false;
  }
  if (image_select_gradient_is_floating(C)) {
    return false;
  }
  if (!image_paint_selection_poll(C)) {
    return false;
  }
  if (!sima->image) {
    return false;
  }
  if (!BKE_image_paint_selection_mask_has_any(sima->image)) {
    CTX_wm_operator_poll_msg_set(C, "No active selection mask");
    return false;
  }
  return true;
}

/** Hit-test the grid_size^2 control points in screen space against a region-local mouse
 * position; returns -1 when none is close enough. */
static int image_select_warp_pick_point_at(const ImageSelectWarpState *state,
                                           const ARegion *region,
                                           const float mval_x,
                                           const float mval_y)
{
  float2 cap_origin_uv, cap_size_uv;
  image_select_warp_capture_uv_rect(state->fragment, &cap_origin_uv, &cap_size_uv);

  constexpr float hit_radius_px = 15.0f;
  for (int i = 0; i < state->tgt_pts.size(); i++) {
    const float2 uv = cap_origin_uv + state->tgt_pts[i] * cap_size_uv;
    float sx, sy;
    ui::view2d_view_to_region_fl(&region->v2d, uv.x, uv.y, &sx, &sy);
    const float dx = mval_x - sx;
    const float dy = mval_y - sy;
    if (std::sqrt(dx * dx + dy * dy) < hit_radius_px) {
      return i;
    }
  }
  return -1;
}

/** Hit-test against a #wmEvent's region-local mouse position (LEFTMOUSE click handling). */
static int image_select_warp_pick_point(const ImageSelectWarpState *state,
                                        const ARegion *region,
                                        const wmEvent *event)
{
  return image_select_warp_pick_point_at(
      state, region, float(event->mval[0]), float(event->mval[1]));
}

/** Snapshot the current tgt_pts and begin dragging control point \a idx. */
static void image_select_warp_begin_drag(ImageSelectWarpState *state, const int idx)
{
  state->drag_position_history.append(state->tgt_pts);
  state->drag_point_idx = idx;
  state->hover_point_idx = -1;
  state->is_dragging = true;
}

/** Paint-cursor poll: only run the hover detector while this space's warp session is floating. */
static bool image_select_warp_paintcursor_poll(bContext *C)
{
  return image_select_warp_is_floating(C);
}

/**
 * Paint-cursor callback used purely as a lightweight hover detector -- it draws nothing itself.
 *
 * A modal operator that stays RUNNING_MODAL for events it does not otherwise consume still forces
 * #WM_HANDLER_BREAK (see `wm_handler_operator_call()`'s "Modal unhandled, break" case in
 * wm_event_system.cc): `wm_event_do_handlers()` skips ALL area/region event dispatch --
 * including View2D pan/zoom and UI widget clicks -- for any event reaching a still-running modal
 * handler, regardless of #OPERATOR_PASS_THROUGH. A previous version of this file kept a modal
 * handler running continuously while warp was floating (to track hover) and that broke
 * navigation and UI interaction while the tool was active.
 *
 * A paint cursor is not a modal operator: #wm_paintcursor_test() runs alongside normal event
 * dispatch without ever setting #WM_HANDLER_BREAK, so it can update #hover_point_idx and request
 * a redraw without blocking anything. The actual grid/handle rendering -- and applying a pending
 * "Grid Size" tool-setting change (#image_paint_warp_settings_revision) -- stays in
 * draw_select_warp_preview() (registered separately via #ED_region_draw_cb_activate), which reads
 * #hover_point_idx set here. Grid-size changes are applied there rather than here because that
 * callback runs on every actual redraw regardless of mouse position, while this one only runs
 * while the mouse hovers the region.
 */
static void image_select_warp_paintcursor_draw(bContext *C,
                                               const int2 &xy,
                                               const float2 & /*tilt*/,
                                               void *customdata)
{
  auto *state = static_cast<ImageSelectWarpState *>(customdata);
  SpaceImage *sima = CTX_wm_space_image(C);
  /* This cursor is registered for every Image Editor WINDOW region; only touch the session it
   * actually belongs to (relevant when more than one Image Editor has a warp floating). */
  if (!sima || sima != state->owner_sima || state->is_dragging) {
    return;
  }
  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return;
  }
  const float mval_x = float(xy[0] - region->winrct.xmin);
  const float mval_y = float(xy[1] - region->winrct.ymin);
  const int idx = image_select_warp_pick_point_at(state, region, mval_x, mval_y);
  if (idx != state->hover_point_idx) {
    state->hover_point_idx = idx;
    ED_region_tag_redraw(region);
  }
}

/**
 * Modal handler for an active drag gesture only: added when a control point is picked up and
 * removed the moment it is released or cancelled (mirrors move/transform/gradient) so navigation
 * and UI interaction are completely normal at all other times.
 */
static wmOperatorStatus image_select_warp_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }
  ImageSelectWarpState *&state_ref = sima->runtime->paint_select.warp;
  ImageSelectWarpState *state = state_ref;
  if (!state || !state->is_dragging) {
    /* Another operator ended the drag (or the whole session) behind our back; the cursor set when
     * the drag started must still be restored. */
    image_select_floating_drag_end(C, state);
    image_select_floating_status_clear(C);
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }
  /* Never dereferenced without this check: the region can disappear mid-drag when the area is
   * closed or split. */
  ARegion *region = CTX_wm_region(C);
  if (!region) {
    image_select_floating_drag_end(C, state);
    state->drag_point_idx = -1;
    image_select_floating_status_clear(C);
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }

  if (event->type == EVT_MODAL_MAP) {
    if (event->val == IMAGE_SELECT_FLOATING_MODAL_CANCEL) {
      image_select_floating_drag_end(C, state);
      image_select_floating_status_clear(C);
      image_select_fragment_restore_source(
          C, sima->image, state->iuser, {state->fragment}, state->undo_begun);
      image_select_warp_state_free(state);
      state_ref = nullptr;
      op->customdata = nullptr;
      ED_region_tag_redraw(region);
      return OPERATOR_CANCELLED;
    }
    /* Confirm / undo-step are not drag-scoped; let the keymap-bound
     * #PAINT_OT_image_select_warp_confirm / `_undo_step` operators handle them. */
    return OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL;
  }

  switch (event->type) {
    case MOUSEMOVE: {
      float uv_x, uv_y;
      ui::view2d_region_to_view(
          &region->v2d, float(event->mval[0]), float(event->mval[1]), &uv_x, &uv_y);
      float2 cap_origin_uv, cap_size_uv;
      image_select_warp_capture_uv_rect(state->fragment, &cap_origin_uv, &cap_size_uv);
      state->tgt_pts[state->drag_point_idx] = (float2(uv_x, uv_y) - cap_origin_uv) / cap_size_uv;
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        image_select_floating_drag_end(C, state);
        state->drag_point_idx = -1;
        image_select_floating_status_clear(C);
        op->customdata = nullptr;
        ED_region_tag_redraw(region);
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;

    default:
      return OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL;
  }
}

static void image_select_warp_cancel(bContext *C, wmOperator *op)
{
  image_select_floating_drag_end(C, nullptr);
  image_select_floating_status_clear(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima && sima->runtime) {
    ImageSelectWarpState *&state_ref = sima->runtime->paint_select.warp;
    if (state_ref) {
      image_select_fragment_restore_source(
          C, sima->image, state_ref->iuser, {state_ref->fragment}, state_ref->undo_begun);
      image_select_warp_state_free(state_ref);
      state_ref = nullptr;
    }
  }
  op->customdata = nullptr;
  ARegion *region = CTX_wm_region(C);
  if (region) {
    ED_region_tag_redraw(region);
  }
}

static wmOperatorStatus image_select_warp_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectWarpState *&state_ref = sima->runtime->paint_select.warp;

  /* Already floating in this space: hit-test a point and start a re-drag. */
  if (state_ref && state_ref->owner_sima == sima) {
    ARegion *region = CTX_wm_region(C);
    const int idx = region ? image_select_warp_pick_point(state_ref, region, event) : -1;
    if (idx == -1) {
      return OPERATOR_PASS_THROUGH;
    }
    image_select_warp_begin_drag(state_ref, idx);
    op->customdata = state_ref;
    WM_event_add_modal_handler(C, op);
    WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
    image_select_floating_status_set(C, op->type, false);
    ED_region_tag_redraw(region);
    return OPERATOR_RUNNING_MODAL;
  }

  /* Another tool may still have a session floating in this space. It has to be ended before
   * #image_select_fragment_lift_source below opens an undo step, which would otherwise free the
   * step that session still believes it owns. See #image_select_floating_sessions_end. */
  image_select_floating_sessions_end(C,
                                     sima,
                                     IMAGE_SELECT_FLOATING_TOOL_MOVE |
                                         IMAGE_SELECT_FLOATING_TOOL_TRANSFORM |
                                         IMAGE_SELECT_FLOATING_TOOL_GRADIENT);

  Image *ima = sima->image;
  if (!ima) {
    return OPERATOR_CANCELLED;
  }
  const int tile_number = image_paint_selection_resolve_tile(ima, sima, sima->iuser.tile);

  auto *state = MEM_new<ImageSelectWarpState>(__func__);
  state->owner_sima = sima;
  state->iuser = sima->iuser;
  state->tile_number = tile_number;
  state->iuser.tile = tile_number;

  const Scene *scene = CTX_data_scene(C);
  state->grid_size = std::clamp(int(scene->toolsettings->imapaint.warp_grid_size),
                                IMAGE_SELECT_WARP_GRID_MIN,
                                IMAGE_SELECT_WARP_GRID_MAX);
  state->applied_settings_revision = image_paint_warp_settings_revision;
  state->interp = eImagePaint_WarpInterpolation(scene->toolsettings->imapaint.warp_interpolation);

  /* Resolved before any state is committed: without a region there is nowhere to register the
   * preview draw callback, so the session could never be seen or dismissed (reachable when the
   * operator is called from Python or a menu outside an Image Editor region). */
  ARegion *region = CTX_wm_region(C);
  if (!region || !region->runtime->type) {
    MEM_delete(state);
    return OPERATOR_CANCELLED;
  }

  if (!image_select_warp_extract(op, ima, state->iuser, tile_number, &state->fragment)) {
    MEM_delete(state);
    return OPERATOR_CANCELLED;
  }
  image_select_warp_init_grid(state);

  image_select_fragment_lift_source(
      C, ima, state->iuser, {state->fragment}, "Warp Selection", state->undo_begun);

  state->owner_region_type = region->runtime->type;
  state->draw_handle = ED_region_draw_cb_activate(
      state->owner_region_type, draw_select_warp_preview, state, REGION_DRAW_POST_PIXEL);
  /* Hover-only detector; does not draw, does not require a running modal operator (see the
   * comment on image_select_warp_paintcursor_draw() for why that matters). */
  state->paint_cursor = WM_paint_cursor_activate(SPACE_IMAGE,
                                                 RGN_TYPE_WINDOW,
                                                 image_select_warp_paintcursor_poll,
                                                 image_select_warp_paintcursor_draw,
                                                 state);

  state_ref = state;
  ED_region_tag_redraw(region);

  /* Only start a drag immediately when invoked by an LMB click on a control point (mirrors
   * image_select_move_invoke's keyboard-vs-mouse invoke distinction). */
  if (event->type == LEFTMOUSE) {
    const int idx = image_select_warp_pick_point(state, region, event);
    if (idx != -1) {
      image_select_warp_begin_drag(state, idx);
      op->customdata = state;
      WM_event_add_modal_handler(C, op);
      WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
      image_select_floating_status_set(C, op->type, false);
      return OPERATOR_RUNNING_MODAL;
    }
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_warp(wmOperatorType *ot)
{
  ot->name = "Warp Selection";
  ot->idname = "PAINT_OT_image_select_warp";
  ot->description =
      "Locally deform the pixels inside the active selection via a draggable control grid";

  ot->invoke = image_select_warp_invoke;
  ot->modal = image_select_warp_modal;
  ot->cancel = image_select_warp_cancel;
  ot->poll = image_select_warp_poll;

  /* No OPTYPE_UNDO, see #PAINT_OT_image_select_move and #PAINT_OT_image_select_transform: the
   * modal outlives the undo step it opened, so letting the WM push on FINISHED would finalize a
   * step this operator no longer owns. */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name State lifecycle
 * \{ */

void image_select_warp_state_free(ImageSelectWarpState *state)
{
  if (!state) {
    return;
  }
  image_select_floating_draw_handle_clear(*state);
  if (state->paint_cursor) {
    /* If the WM's own runtime is already gone, full application exit is tearing down IDs in
     * reverse index order, so #wmWindowManager was freed before this state's owning Image; its
     * paint cursor list -- including this handle -- was already destroyed with it. Touching it
     * again would double-free/null-deref (see #WM_paint_cursor_end). */
    if (!G_MAIN->wm.is_empty()) {
      WM_paint_cursor_end(state->paint_cursor);
    }
    state->paint_cursor = nullptr;
  }
  selection_tile_fragment_free(state->fragment);
  MEM_delete(state);
}

/** \} */

} /* namespace blender */
