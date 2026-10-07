/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Warp Selection in the 3D Viewport: a draggable control grid over the floating fragment, drawn
 * on the surface. Grid points live on the anchor plane and map into the fragment's capture space
 * through the anchor's plane->UV Jacobian, so dragging a point moves it along the surface and the
 * committed warp is the correctly projected deformation. One fragment per target: the primary
 * target drives the shared grid and every Material canvas is warped with it, like the Image
 * Editor's single-tile warp.
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_windowmanager_types.h"

#include "DEG_depsgraph_query.hh"

#include "BKE_context.hh"

#include "DEG_depsgraph.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_library.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "GEO_reverse_uv_sampler.hh"
#include "paint_image_select_floating.hh"
#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_view3d.hh"
#include "paint_image_select_warp_intern.hh"
#include "../mesh/sculpt_intern.hh"

#include "BKE_mesh.hh"
#include "BKE_screen.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Warp math
 *
 * Grid points are stored in the fragment's capture-local pixel space (y-up, row 0 = bottom, the
 * same convention the shared affine writer uses). Evaluations interpolate the deformed positions
 * (\a tgt) or the undeformed ones (\a src) over normalized grid coordinates [0..1]².
 * \{ */

namespace image_select_v3d {

/** Catmull-Rom spline value at \a t for one scalar component, with linearly extrapolated
 * phantom border points (C1-continuous; reproduces affine fields at the edges). */
inline float warp_catmull_rom(const float p0, const float p1, const float p2, const float p3, const float t)
{
  const float t2 = t * t;
  const float t3 = t2 * t;
  return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                 (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                 (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

/** Catmull-Rom over four samples with parameter \a t in [0..1]. */
inline float2 warp_cr4(const float2 &p0, const float2 &p1, const float2 &p2, const float2 &p3, const float t)
{
  return float2(warp_catmull_rom(p0.x, p1.x, p2.x, p3.x, t),
                warp_catmull_rom(p0.y, p1.y, p2.y, p3.y, t));
}

/** One-dimensional Catmull-Rom over a grid line of \a n points, with linearly extrapolated
 * phantom samples beyond the borders. \a fetch receives the raw (possibly out-of-range) index. */
template<typename Fetch>
inline float2 warp_smooth_1d(const Fetch &fetch, const float coord, const int n)
{
  const float t = coord * float(n - 1);
  const int i0 = std::clamp(int(std::floor(t)), 0, n - 2);
  const float f = t - float(i0);
  const auto sample = [&](const int i) {
    if (i < 0) {
      const float2 a = fetch(0);
      const float2 b = fetch(1);
      return a + (a - b) * float(-i);
    }
    if (i > n - 1) {
      const float2 a = fetch(n - 1);
      const float2 b = fetch(n - 2);
      return a + (a - b) * float(i - (n - 1));
    }
    return fetch(i);
  };
  return warp_cr4(sample(i0 - 1), sample(i0), sample(i0 + 1), sample(i0 + 2), f);
}

/** Tensor-product smooth (Catmull-Rom) grid evaluation at normalized [0..1]² coordinates. */
inline float2 warp_grid_eval_smooth(const Array<float2> &pts, const int grid_size, const float2 &q)
{
  const float u = std::clamp(q.x, 0.0f, 1.0f);
  const float v = std::clamp(q.y, 0.0f, 1.0f);
  /* Rows around v, each smoothly evaluated along x; phantom rows beyond the borders
   * extrapolate the border rows linearly. */
  const auto row_linear = [&](const int j) {
    const auto fetch = [&](const int i) { return pts[std::clamp(j, 0, grid_size - 1) * grid_size + i]; };
    return warp_smooth_1d(fetch, u, grid_size);
  };
  const float t = v * float(grid_size - 1);
  const int j0 = std::clamp(int(std::floor(t)), 0, grid_size - 2);
  const float fv = t - float(j0);
  const auto row_j = [&](const int j) {
    if (j < 0) {
      return row_linear(0) * 2.0f - row_linear(1);
    }
    if (j > grid_size - 1) {
      return row_linear(grid_size - 1) * 2.0f - row_linear(grid_size - 2);
    }
    return row_linear(j);
  };
  return warp_cr4(row_j(j0 - 1), row_j(j0), row_j(j0 + 1), row_j(j0 + 2), fv);
}

/** Bilinear evaluation of the containing cell at continuous grid coordinates (in
 * [0, grid_size-1]²); reproduces the control points exactly at integer coordinates. */
inline float2 warp_grid_eval_linear(const Array<float2> &pts, const int grid_size, const float2 &g)
{
  const float gx = std::clamp(g.x, 0.0f, float(grid_size - 1));
  const float gy = std::clamp(g.y, 0.0f, float(grid_size - 1));
  const int cell_x = std::clamp(int(gx), 0, grid_size - 2);
  const int cell_y = std::clamp(int(gy), 0, grid_size - 2);
  const float u = gx - float(cell_x);
  const float v = gy - float(cell_y);
  const float2 p00 = pts[cell_y * grid_size + cell_x];
  const float2 p10 = pts[cell_y * grid_size + cell_x + 1];
  const float2 p01 = pts[(cell_y + 1) * grid_size + cell_x];
  const float2 p11 = pts[(cell_y + 1) * grid_size + cell_x + 1];
  return p00 + (p10 - p00) * u + (p01 - p00) * v + (p11 - p10 - p01 + p00) * (u * v);
}

/** Tessellation density shared with the Image Editor's warp: sub-quads per control cell. */
constexpr int WARP3D_CELL_SUBDIV = 8;

/** Evaluate a point on the control grid at continuous grid coordinates, through the tool's
 * Interpolation setting (the same contract as the Image Editor's warp: LINEAR = piecewise
 * bilinear, SMOOTH = Catmull-Rom). */
inline float2 warp_grid_eval(const Array<float2> &pts,
                             const int grid_size,
                             const float2 &g,
                             const int interp)
{
  if (interp == IMAGE_PAINT_WARP_INTERP_SMOOTH) {
    /* #warp_grid_eval_smooth takes normalized [0..1]² coordinates. */
    const float2 q = grid_size > 1 ? g / float(grid_size - 1) : float2(0.0f);
    return warp_grid_eval_smooth(pts, grid_size, q);
  }
  return warp_grid_eval_linear(pts, grid_size, g);
}

/** Cells each control cell is subdivided into for the preview / commit tessellation: 1 for LINEAR
 * (the raw control points are already exact), the shared subdivision density for SMOOTH. */
inline int warp_cell_subdiv(const int interp)
{
  return interp == IMAGE_PAINT_WARP_INTERP_SMOOTH ? WARP3D_CELL_SUBDIV : 1;
}

}  // namespace image_select_v3d

/* The warp math is called from the preview, the settings re-apply and the commit writer, which
 * all live at this file's outer scope; unqualified lookup does not descend into the nested
 * namespace (the same import pattern the transform tool uses for its handle types). */
using image_select_v3d::warp_cell_subdiv;
using image_select_v3d::warp_grid_eval;

/** \} */

/* -------------------------------------------------------------------- */
/** \name PBR preview channels
 *
 * The on-surface preview composites the four maps a PBR material is painted through (the paint
 * system's default visible channels). Other lifted channels are warped on commit but cannot be
 * meaningfully shaded into the patch, so they are not previewed.
 * \{ */

enum Warp3DPreviewChannel {
  WARP3D_PREVIEW_BASE_COLOR = 0,
  WARP3D_PREVIEW_ROUGHNESS = 1,
  WARP3D_PREVIEW_METALLIC = 2,
  WARP3D_PREVIEW_NORMAL = 3,
  WARP3D_PREVIEW_CHANNEL_NUM = 4,
};

/** Preview slot of a material paint channel, or -1 when the channel is not previewed. */
inline int warp3d_channel_slot(const eMaterialPaintChannel channel)
{
  switch (channel) {
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
      return WARP3D_PREVIEW_BASE_COLOR;
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
      return WARP3D_PREVIEW_ROUGHNESS;
    case PAINT_MATERIAL_CHANNEL_METALLIC:
      return WARP3D_PREVIEW_METALLIC;
    case PAINT_MATERIAL_CHANNEL_NORMAL:
      return WARP3D_PREVIEW_NORMAL;
    default:
      return -1;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name State
 * \{ */

struct ImageSelectWarp3DState : PaintSelectView3DFloatingSession {
  ImageSelectView3DAnchor anchor;
  Image *ima = nullptr;
  ImageUser iuser = {};
  /** Material canvas: the primary target's channel. The primary drives the shared grid and its
   * capture space is what the grid coordinates live in. */
  eMaterialPaintChannel primary_channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  /** The single warped fragment (primary target, drives the shared grid and the overlay). */
  SelectionTileFragment fragment = {};
  bool has_fragment = false;
  /** One fragment per remaining Material target: the same selection on the other PBR maps, warped
   * with the shared grid so all canvases stay in sync. */
  struct WarpExtraTarget {
    Image *ima = nullptr;
    ImageUser iuser = {};
    eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
    SelectionTileFragment fragment = {};
    bool has_fragment = false;
  };
  Vector<WarpExtraTarget> extra_targets;
  int grid_size = 4;
  int interp = IMAGE_PAINT_WARP_INTERP_LINEAR;
  /** Capture-local pixel space (y-up): undeformed and deformed grid points. */
  Array<float2> src_pts;
  Array<float2> tgt_pts;
  Vector<Array<float2>> drag_history;
  ImageSelectView3DSurfaceMap surface_map;
  int drag_point = -1;
  int hover_point = -1;
  /** Screen positions of the grid points, refreshed by the draw callback. */
  Vector<float2> screen_pts;
  bool screen_valid = false;
  /** GPU copies of the channel fragments' display buffers for the on-surface preview, one slot
   * per previewed PBR channel (see #Warp3DPreviewChannel), created lazily by the texture draw
   * callback (a GPU context is current there). A Material canvas lifts one fragment per PBR map:
   * the preview composites the maps it finds into a shaded PBR patch, and binds a neutral 1x1
   * stand-in per missing channel. */
  gpu::Texture *preview_textures[WARP3D_PREVIEW_CHANNEL_NUM] = {nullptr};
  gpu::Texture *preview_default_textures[WARP3D_PREVIEW_CHANNEL_NUM] = {nullptr};
  /** Flat-path texture of the single-target session (a channel-slot-independent copy of the
   * primary fragment's display buffer; the channel slots are the PBR composite's). */
  gpu::Texture *preview_texture_flat = nullptr;
  /** POST_VIEW draw handle of the texture preview; #draw_handle (POST_PIXEL) owns the grid. */
  void *draw_handle_texture = nullptr;

  explicit ImageSelectWarp3DState()
      : PaintSelectView3DFloatingSession(PaintSelectView3DTool::Warp)
  {
  }

  ~ImageSelectWarp3DState() override
  {
    for (gpu::Texture *tex : this->preview_textures) {
      image_select_view3d_preview_texture_free(tex);
    }
    for (gpu::Texture *tex : this->preview_default_textures) {
      image_select_view3d_preview_texture_free(tex);
    }
    image_select_view3d_preview_texture_free(this->preview_texture_flat);
    if (this->has_fragment) {
      image_select_fragment_free(this->fragment);
      this->has_fragment = false;
    }
    for (WarpExtraTarget &target : this->extra_targets) {
      if (target.has_fragment) {
        image_select_fragment_free(target.fragment);
        target.has_fragment = false;
      }
    }
  }

  void commit(bContext *C) override;
  void cancel(bContext *C) override;
  bool undo_step() override;
  bool cursor_over_fragment(const bContext *C, const int2 &mval) const override;

  /** Fragment of \a slot's preview channel, or null when the material does not lift it. */
  SelectionTileFragment *preview_fragment_of_slot(int slot)
  {
    if (slot < 0 || slot >= WARP3D_PREVIEW_CHANNEL_NUM) {
      return nullptr;
    }
    if (warp3d_channel_slot(this->primary_channel) == slot) {
      return this->has_fragment ? &this->fragment : nullptr;
    }
    for (WarpExtraTarget &target : this->extra_targets) {
      if (warp3d_channel_slot(target.channel) == slot) {
        return target.has_fragment ? &target.fragment : nullptr;
      }
    }
    return nullptr;
  }

  /** Capture-local px -> global UV. */
  float2 capture_px_to_uv(const float2 &cap_px) const;
  /** Global UV -> capture-local px. */
  float2 uv_to_capture_px(const float2 &uv) const;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Coordinate mapping and grid setup
 * \{ */

float2 ImageSelectWarp3DState::capture_px_to_uv(const float2 &cap_px) const
{
  const float2 tile_uv = image_select_udim_tile_uv_origin(this->fragment.geom.tile_number);
  return tile_uv + (float2(this->fragment.geom.origin_px) + cap_px) /
                       float2(this->fragment.geom.tile_size_px);
}

float2 ImageSelectWarp3DState::uv_to_capture_px(const float2 &uv) const
{
  const float2 tile_uv = image_select_udim_tile_uv_origin(this->fragment.geom.tile_number);
  return (uv - tile_uv) * float2(this->fragment.geom.tile_size_px) -
         float2(this->fragment.geom.origin_px);
}

static void warp3d_free_draw_handle(ImageSelectWarp3DState &state)
{
  image_select_view3d_floating_draw_handle_clear(state);
  if (state.draw_handle_texture != nullptr && state.owner_region_type != nullptr) {
    ED_region_draw_cb_exit(state.owner_region_type, state.draw_handle_texture);
    state.draw_handle_texture = nullptr;
  }
}

/** First tile with any selection, captured with the Image Editor warp's margin contract
 * (#image_select_warp_extract): the tight selection bbox expanded by max(half the tight bbox
 * size, IMAGE_SELECT_WARP_MIN_MARGIN_PX) per side, clamped to the tile. Unlike Move / Transform's
 * #image_select_extract_per_tile (a feather-radius pad), this margin is real canvas content the
 * deformed selection grows into when its grid points are dragged outward -- with a tight capture
 * the commit would clip the warped fragment at the pre-warp selection bounds instead of letting
 * the selection become larger. */
static bool warp3d_extract_first_tile(wmOperator *op,
                                      Image *ima,
                                      const ImageUser &base_iuser,
                                      SelectionTileFragment *r_fragment)
{
  for (ImageTile *tile : ListBaseWrapper<ImageTile>(ima->tiles)) {
    if (image_select_warp_extract(op, ima, base_iuser, tile->tile_number, false, r_fragment)) {
      return true;
    }
  }
  return false;
}

/** Even grid over the tight selection rect, in capture-local px. */
static void warp3d_init_grid(ImageSelectWarp3DState &state)
{
  const int n = state.grid_size;
  state.src_pts = Array<float2>(n * n);
  state.tgt_pts = Array<float2>(n * n);
  /* Capture-local px (0 = the fragment's origin). #image_select_warp_extract stores
   * #selection_origin_px relative to the capture origin (the same convention the 2D warp uses),
   * so it is the grid's lo corner directly. */
  const int2 lo = state.fragment.geom.selection_origin_px;
  const int2 size = state.fragment.geom.selection_size_px;
  for (const int j : IndexRange(n)) {
    for (const int i : IndexRange(n)) {
      const float2 p(lo.x + size.x * (float(i) / float(n - 1)),
                     lo.y + size.y * (float(j) / float(n - 1)));
      state.src_pts[j * n + i] = p;
      state.tgt_pts[j * n + i] = p;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Screen mapping, draw and hit-tests
 * \{ */

static bool warp3d_capture_px_to_screen(ImageSelectWarp3DState &state,
                                        const ARegion &region,
                                        const float2 &cap_px,
                                        float2 &r_screen)
{
  const Object *ob = state.anchor.object;
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region.regiondata);
  if (ob == nullptr || rv3d == nullptr || !state.surface_map.sampler) {
    return false;
  }
  const Mesh *mesh = id_cast<const Mesh *>(ob->data);
  if (mesh == nullptr) {
    return false;
  }
  const float2 uv = state.capture_px_to_uv(cap_px);
  float3 co, no;
  if (!state.surface_map.sample(mesh->corner_verts(), mesh->vert_positions(), uv, co, no)) {
    return false;
  }
  const float3 world = math::transform_point(float4x4(ob->object_to_world()), co);
  const float4 proj = float4x4(rv3d->persmat) * float4(world.x, world.y, world.z, 1.0f);
  if (proj.w <= 0.0f) {
    return false;
  }
  r_screen = float2((proj.x / proj.w * 0.5f + 0.5f) * float(region.winx),
                    (proj.y / proj.w * 0.5f + 0.5f) * float(region.winy));
  return true;
}

static void warp3d_refresh_screen(ImageSelectWarp3DState &state, const ARegion &region)
{
  state.screen_valid = false;
  state.screen_pts.clear();
  if (!state.surface_map.sampler) {
    return;
  }
  state.screen_pts.resize(state.tgt_pts.size());
  int ok = 0;
  for (const int i : state.tgt_pts.index_range()) {
    if (warp3d_capture_px_to_screen(state, region, state.tgt_pts[i], state.screen_pts[i])) {
      ok++;
    }
  }
  state.screen_valid = ok == int(state.tgt_pts.size());
}

static int warp3d_pick_point(const ImageSelectWarp3DState &state, const int2 &mval)
{
  if (!state.screen_valid) {
    return -1;
  }
  int best = -1;
  float best_dist = 15.0f;
  for (const int i : state.screen_pts.index_range()) {
    const float d = math::distance(float2(mval), state.screen_pts[i]);
    if (d <= best_dist) {
      best_dist = d;
      best = i;
    }
  }
  return best;
}

/** Capture-local px -> global UV of one target's fragment (the shared grid is scaled into each
 * target's own capture space, like the commit writer does). */
static float2 warp_frag_capture_px_to_uv(const SelectionTileFragment &frag, const float2 &cap_px)
{
  const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
  return tile_uv + (float2(frag.geom.origin_px) + cap_px) / float2(frag.geom.tile_size_px);
}

/** Lazy GPU copy of one preview channel's fragment display buffer. */
static gpu::Texture *warp3d_channel_texture(ImageSelectWarp3DState &state,
                                            const int slot,
                                            SelectionTileFragment &frag)
{
  gpu::Texture *&texture = state.preview_textures[slot];
  if (texture == nullptr && frag.preview.fragment_display_ibuf != nullptr) {
    texture = IMB_create_gpu_texture(
        "Warp3DFragment", frag.preview.fragment_display_ibuf, true, false, false);
    if (texture != nullptr) {
      /* Linear, not nearest: see #move3d_draw (heavy minification on the surface). */
      GPU_texture_filter_mode(texture, true);
    }
  }
  return texture;
}

/** Neutral 1x1 stand-in bound for a PBR channel the material does not lift. */
static gpu::Texture *warp3d_channel_default_texture(ImageSelectWarp3DState &state, const int slot)
{
  gpu::Texture *&texture = state.preview_default_textures[slot];
  if (texture == nullptr) {
    static const float defaults[WARP3D_PREVIEW_CHANNEL_NUM][4] = {
        /* Base color white, roughness mid, metallic 0, flat tangent-space normal. */
        {1.0f, 1.0f, 1.0f, 1.0f},
        {0.5f, 0.5f, 0.5f, 1.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {0.5f, 0.5f, 1.0f, 1.0f},
    };
    texture = GPU_texture_create_2d("Warp3DFragmentDefault",
                                    1,
                                    1,
                                    1,
                                    gpu::TextureFormat::SFLOAT_16_16_16_16,
                                    GPU_TEXTURE_USAGE_SHADER_READ,
                                    defaults[slot]);
  }
  return texture;
}

/**
 * Texture-coordinate affine of one channel's fragment: maps a global source UV onto the fragment's
 * normalized capture rect. Channels lift the same selection at their own tile resolution (and
 * their own feather padding), so each needs its own transform.
 */
static void warp3d_texco_xform(const SelectionTileFragment &frag, float2 &r_scale, float2 &r_offset)
{
  const float2 frag_size = math::max(float2(frag.geom.size_px), float2(1.0f));
  const float2 tile_size = float2(frag.geom.tile_size_px);
  const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
  r_scale = tile_size / frag_size;
  r_offset = (-tile_uv * tile_size - float2(frag.geom.origin_px)) / frag_size;
}

/**
 * The warped fragment content on the surface (POST_VIEW). The move / transform previews draw an
 * affine rect; the warp's deformation is only known per grid point, so the triangles are built
 * from the grid: deformed (tgt) evaluations mapped onto the surface, textured from the undeformed
 * (src) evaluations, so the content stretches with the drag. LINEAR keeps the raw control points
 * (their bilinear surface); SMOOTH subdivides every control cell through the spline, so the
 * preview shows the same shape the commit writes.
 *
 * A Material canvas lifts one fragment per PBR map. With several targets the preview composites
 * the maps it finds (Base Color / Roughness / Metallic / Normal) into a shaded PBR patch through
 * #GPU_SHADER_IMAGE_SELECT_PBR, so the floating content reads like the material instead of one
 * flat map; a single target keeps the plain textured draw.
 */
static void warp3d_draw_texture(const bContext * /*C*/, ARegion *region, void *arg)
{
  auto *state = static_cast<ImageSelectWarp3DState *>(arg);
  if (state == nullptr || region == nullptr || !state->surface_map.sampler ||
      state->anchor.object == nullptr || !state->has_fragment)
  {
    return;
  }
  const Mesh *mesh = id_cast<const Mesh *>(state->anchor.object->data);
  if (mesh == nullptr) {
    return;
  }
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<float3> vert_positions = mesh->vert_positions();
  const float4x4 object_to_world(state->anchor.object->object_to_world());

  /* Deformed lattice: continuous grid coordinates sampled through the Interpolation setting. The
   * positions are the primary target's mapping (the grid's reference capture space); the commit
   * maps the other targets through the same scaling. */
  const int n = state->grid_size;
  const int subdiv = warp_cell_subdiv(state->interp);
  const int res = (n - 1) * subdiv + 1;
  const float2 frag_size = math::max(float2(state->fragment.geom.size_px), float2(1.0f));
  Array<float3> lattice_world(size_t(res) * res);
  Array<float3> lattice_normal(size_t(res) * res);
  Array<float2> lattice_texco(size_t(res) * res);
  Array<float2> lattice_src_uv(size_t(res) * res);
  Array<bool> lattice_valid(size_t(res) * res);
  for (const int j : IndexRange(res)) {
    for (const int i : IndexRange(res)) {
      const int idx = j * res + i;
      const float2 g(float(i) / float(subdiv), float(j) / float(subdiv));
      const float2 tgt_px = warp_grid_eval(state->tgt_pts, n, g, state->interp);
      float3 co, no;
      const float2 uv_dest = warp_frag_capture_px_to_uv(state->fragment, tgt_px);
      lattice_valid[idx] = state->surface_map.sample(corner_verts, vert_positions, uv_dest, co, no);
      lattice_world[idx] = math::transform_point(object_to_world, co);
      lattice_normal[idx] = math::transform_direction(object_to_world, no);
      const float2 src_px = warp_grid_eval(state->src_pts, n, g, state->interp);
      lattice_texco[idx] = src_px / frag_size;
      lattice_src_uv[idx] = state->capture_px_to_uv(src_px);
    }
  }

  /* Lattice triangles whose four corners all sample; a point whose UV misses the surface drops
   * the quads around it. */
  Vector<int> tris;
  tris.reserve((size_t(res) - 1) * (res - 1) * 6);
  for (const int j : IndexRange(res - 1)) {
    for (const int i : IndexRange(res - 1)) {
      const int i00 = j * res + i;
      const int i10 = j * res + i + 1;
      const int i01 = (j + 1) * res + i;
      const int i11 = (j + 1) * res + i + 1;
      if (!(lattice_valid[i00] && lattice_valid[i10] && lattice_valid[i01] &&
            lattice_valid[i11]))
      {
        continue;
      }
      const int quad[4] = {i00, i10, i11, i01};
      const int quad_tris[2][3] = {{0, 1, 2}, {0, 2, 3}};
      for (const int t : IndexRange(2)) {
        for (const int k : quad_tris[t]) {
          tris.append(quad[k]);
        }
      }
    }
  }
  if (tris.is_empty()) {
    return;
  }

  const bool use_pbr = !state->extra_targets.is_empty();

  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);
  if (rv3d != nullptr) {
    /* The grid triangles lie exactly on the mesh surface: without a depth offset the shared
     * pixels z-fight and the content shows through only in patches. */
    ED_view3d_polygon_offset(rv3d, 1.0f);
  }
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);

  if (use_pbr) {
    const uint texco = GPU_vertformat_attr_add(
        format, "src_uv", gpu::VertAttrType::SFLOAT_32_32);
    const uint nor = GPU_vertformat_attr_add(format, "nor", gpu::VertAttrType::SFLOAT_32_32_32);
    immBindBuiltinProgram(GPU_SHADER_IMAGE_SELECT_PBR);

    gpu::Texture *channel_textures[WARP3D_PREVIEW_CHANNEL_NUM] = {nullptr};
    for (const int slot : IndexRange(WARP3D_PREVIEW_CHANNEL_NUM)) {
      if (SelectionTileFragment *frag = state->preview_fragment_of_slot(slot)) {
        channel_textures[slot] = warp3d_channel_texture(*state, slot, *frag);
      }
      if (channel_textures[slot] == nullptr) {
        channel_textures[slot] = warp3d_channel_default_texture(*state, slot);
      }
    }
    immBindTextureSampler("base_color_tex",
                          channel_textures[WARP3D_PREVIEW_BASE_COLOR],
                          {GPU_SAMPLER_FILTERING_LINEAR,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});
    immBindTextureSampler("roughness_tex",
                          channel_textures[WARP3D_PREVIEW_ROUGHNESS],
                          {GPU_SAMPLER_FILTERING_LINEAR,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});
    immBindTextureSampler("metallic_tex",
                          channel_textures[WARP3D_PREVIEW_METALLIC],
                          {GPU_SAMPLER_FILTERING_LINEAR,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});
    immBindTextureSampler("normal_tex",
                          channel_textures[WARP3D_PREVIEW_NORMAL],
                          {GPU_SAMPLER_FILTERING_LINEAR,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});

    static const char *texco_scale_names[WARP3D_PREVIEW_CHANNEL_NUM] = {
        "texco_scale_base_color",
        "texco_scale_roughness",
        "texco_scale_metallic",
        "texco_scale_normal",
    };
    static const char *texco_offset_names[WARP3D_PREVIEW_CHANNEL_NUM] = {
        "texco_offset_base_color",
        "texco_offset_roughness",
        "texco_offset_metallic",
        "texco_offset_normal",
    };
    for (const int slot : IndexRange(WARP3D_PREVIEW_CHANNEL_NUM)) {
      float2 scale(1.0f, 1.0f);
      float2 offset(0.0f, 0.0f);
      if (SelectionTileFragment *frag = state->preview_fragment_of_slot(slot)) {
        warp3d_texco_xform(*frag, scale, offset);
      }
      immUniform2f(texco_scale_names[slot], scale.x, scale.y);
      immUniform2f(texco_offset_names[slot], offset.x, offset.y);
    }

    /* Headlight with a slight studio offset: the patch shades with the view like the viewport's
     * default studio light, so it reads as the material while the user orbits. */
    float3 view_direction(0.0f, 0.0f, 1.0f);
    float3 light_direction(0.0f, 0.0f, 1.0f);
    if (rv3d != nullptr) {
      view_direction = math::normalize(float3(rv3d->viewinv[2][0],
                                              rv3d->viewinv[2][1],
                                              rv3d->viewinv[2][2]));
      light_direction = math::normalize(float3(rv3d->viewinv[2][0],
                                               rv3d->viewinv[2][1],
                                               rv3d->viewinv[2][2]) +
                                        float3(rv3d->viewinv[1][0],
                                               rv3d->viewinv[1][1],
                                               rv3d->viewinv[1][2]) *
                                            0.4f +
                                        float3(rv3d->viewinv[0][0],
                                               rv3d->viewinv[0][1],
                                               rv3d->viewinv[0][2]) *
                                            0.3f);
    }
    immUniform3f("light_direction", light_direction.x, light_direction.y, light_direction.z);
    immUniform3f("view_direction", view_direction.x, view_direction.y, view_direction.z);

    immBegin(GPU_PRIM_TRIS, uint(tris.size()));
    for (const int idx : tris) {
      immAttr3f(nor,
                lattice_normal[idx].x,
                lattice_normal[idx].y,
                lattice_normal[idx].z);
      immAttr2f(texco, lattice_src_uv[idx].x, lattice_src_uv[idx].y);
      immVertex3f(pos,
                  lattice_world[idx].x,
                  lattice_world[idx].y,
                  lattice_world[idx].z);
    }
    immEnd();
    immUnbindProgram();
  }
  else {
    const uint texco = GPU_vertformat_attr_add(
        format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);
    gpu::Texture *&texture = state->preview_texture_flat;
    if (texture == nullptr && state->fragment.preview.fragment_display_ibuf != nullptr) {
      texture = IMB_create_gpu_texture(
          "Warp3DFragment", state->fragment.preview.fragment_display_ibuf, true, false, false);
      if (texture != nullptr) {
        /* Linear, not nearest: see #move3d_draw (heavy minification on the surface). */
        GPU_texture_filter_mode(texture, true);
      }
    }
    if (texture == nullptr) {
      GPU_blend(GPU_BLEND_NONE);
      if (rv3d != nullptr) {
        ED_view3d_polygon_offset(rv3d, 0.0f);
      }
      return;
    }
    immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
    immUniformColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    immBindTextureSampler("image",
                          texture,
                          {GPU_SAMPLER_FILTERING_LINEAR,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});
    immBegin(GPU_PRIM_TRIS, uint(tris.size()));
    for (const int idx : tris) {
      immAttr2f(texco, lattice_texco[idx].x, lattice_texco[idx].y);
      immVertex3f(pos,
                  lattice_world[idx].x,
                  lattice_world[idx].y,
                  lattice_world[idx].z);
    }
    immEnd();
    immUnbindProgram();
  }
  GPU_blend(GPU_BLEND_NONE);
  if (rv3d != nullptr) {
    ED_view3d_polygon_offset(rv3d, 0.0f);
  }
}

/**
 * Live tool settings, re-read on every redraw like the Image Editor's warp does (the RNA update
 * notifier forces a redraw): Interpolation takes effect on the next tessellation; a Grid Size
 * change resamples both grids through their own surface so the deformation survives.
 */
static void warp3d_apply_settings(ImageSelectWarp3DState &state, const bContext *C)
{
  if (state.is_dragging) {
    return;
  }
  const Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return;
  }
  state.interp = int(scene->toolsettings->imapaint.warp_interpolation);
  const int new_grid_size = std::clamp(int(scene->toolsettings->imapaint.warp_grid_size), 2, 10);
  if (new_grid_size == state.grid_size) {
    return;
  }
  const int old_grid_size = state.grid_size;
  Array<float2> new_src(size_t(new_grid_size) * new_grid_size);
  Array<float2> new_tgt(size_t(new_grid_size) * new_grid_size);
  for (const int j : IndexRange(new_grid_size)) {
    for (const int i : IndexRange(new_grid_size)) {
      const float2 g(float(i) / float(new_grid_size - 1) * float(old_grid_size - 1),
                     float(j) / float(new_grid_size - 1) * float(old_grid_size - 1));
      new_src[j * new_grid_size + i] = warp_grid_eval(
          state.src_pts, old_grid_size, g, state.interp);
      new_tgt[j * new_grid_size + i] = warp_grid_eval(
          state.tgt_pts, old_grid_size, g, state.interp);
    }
  }
  state.grid_size = new_grid_size;
  state.src_pts = std::move(new_src);
  state.tgt_pts = std::move(new_tgt);
  /* The snapshots are sized for the old grid; restoring one would corrupt the points. */
  state.drag_history.clear();
  state.drag_point = -1;
  state.hover_point = -1;
}

static void warp3d_draw(const bContext *C, ARegion *region, void *arg)
{
  auto *state = static_cast<ImageSelectWarp3DState *>(arg);
  if (state == nullptr || region == nullptr || state->anchor.object == nullptr) {
    return;
  }
  warp3d_apply_settings(*state, C);
  warp3d_refresh_screen(*state, *region);

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_depth_test(GPU_DEPTH_NONE);
  const uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos",
                                           gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  const int n = state->grid_size;
  if (state->screen_valid) {
    /* Grid wireframe through the deformed points (surface-projected). */
    immUniformColor4f(1.0f, 1.0f, 1.0f, 0.5f);
    immBegin(GPU_PRIM_LINES, uint(n * (n - 1) * 4));
    for (const int j : IndexRange(n)) {
      for (const int i : IndexRange(n)) {
        if (i + 1 < n) {
          const float2 &a = state->screen_pts[j * n + i];
          const float2 &b = state->screen_pts[j * n + i + 1];
          immVertex3f(pos, a.x, a.y, 0.0f);
          immVertex3f(pos, b.x, b.y, 0.0f);
        }
        if (j + 1 < n) {
          const float2 &a = state->screen_pts[j * n + i];
          const float2 &b = state->screen_pts[(j + 1) * n + i];
          immVertex3f(pos, a.x, a.y, 0.0f);
          immVertex3f(pos, b.x, b.y, 0.0f);
        }
      }
    }
    immEnd();

    /* Handles: red while dragging, yellow otherwise. The vertex count must cover every emitted
     * vertex: 8 per handle (4 lines, 2 vertices each) -- an under-count draws only the first
     * handles and writes past the immediate buffer. */
    immUniformColor4f(state->drag_point >= 0 ? 1.0f : 1.0f,
                      state->drag_point >= 0 ? 0.2f : 0.85f,
                      state->drag_point >= 0 ? 0.2f : 0.0f,
                      0.95f);
    const float s = 4.0f;
    immBegin(GPU_PRIM_LINES, uint(state->screen_pts.size()) * 8);
    for (const float2 &p : state->screen_pts) {
      immVertex3f(pos, p.x - s, p.y - s, 0.0f);
      immVertex3f(pos, p.x + s, p.y - s, 0.0f);
      immVertex3f(pos, p.x + s, p.y - s, 0.0f);
      immVertex3f(pos, p.x + s, p.y + s, 0.0f);
      immVertex3f(pos, p.x + s, p.y + s, 0.0f);
      immVertex3f(pos, p.x - s, p.y + s, 0.0f);
      immVertex3f(pos, p.x - s, p.y + s, 0.0f);
      immVertex3f(pos, p.x - s, p.y - s, 0.0f);
    }
    immEnd();
  }
  immUnbindProgram();
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Commit
 * \{ */

/**
 * Deformed write-back for one target, in place over the fragment's capture region. For every
 * canvas pixel in a deformed triangle's footprint: barycentric map to the source position, sample
 * the fragment (feather / binary mask weight), blend into the canvas. Pixels inside the original
 * selection that the deformed content does not reach are restored from the fragment backup (the
 * same "keep original outside the deformed content" contract as the Image Editor's warp). The
 * shared grid is defined in the primary target's capture pixels and scaled into this target's
 * capture pixels (identity for the primary target itself), so every Material canvas receives the
 * same deformation.
 */
static void warp3d_write_final_target(bContext *C,
                                      ImageSelectWarp3DState &state,
                                      Image &ima,
                                      const ImageUser &iuser,
                                      const SelectionTileFragment &frag)
{
  image_select_fragment_lift_source(C, &ima, iuser, Span(&frag, 1));

  if (frag.pixels.fragment_ibuf == nullptr) {
    return;
  }
  const int fw = frag.geom.size_px.x;
  const int fh = frag.geom.size_px.y;
  const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
  const float2 tile_size = float2(frag.geom.tile_size_px);
  const int n = state.grid_size;

  /* The shared grid lives in the primary target's capture pixels; scaling by the selection sizes
   * maps it into this target's own capture space. The max() guards a degenerate selection on the
   * primary target, which would otherwise divide by zero and poison the grid with NaNs. */
  const float2 grid_scale = float2(frag.geom.selection_size_px) /
                            math::max(float2(state.fragment.geom.selection_size_px),
                                      float2(1.0f));

  const auto capture_px_to_uv = [&](const float2 &cap_px) {
    return tile_uv + (float2(frag.geom.origin_px) + cap_px) / float2(frag.geom.tile_size_px);
  };

  /* Grid triangles in capture-local px: deformed (tgt) corners paired with undeformed (src). The
   * grid is evaluated through the Interpolation setting on the raw (primary-space) points, then
   * scaled into this target's space. LINEAR reproduces the raw control points exactly (the
   * previous corner-based tessellation); SMOOTH subdivides every control cell through the
   * Catmull-Rom spline, so the commit matches the on-surface preview. */
  const int subdiv = warp_cell_subdiv(state.interp);
  const auto eval_tgt = [&](const float2 &g) {
    return warp_grid_eval(state.tgt_pts, n, g, state.interp) * grid_scale;
  };
  const auto eval_src = [&](const float2 &g) {
    return warp_grid_eval(state.src_pts, n, g, state.interp) * grid_scale;
  };

  struct WarpTri {
    float2 tgt[3];
    float2 src[3];
    /** Turns the stored vectors of a normal map with the triangle's local rotation. */
    float2x2 normal_rotation = float2x2::identity();
  };
  Vector<WarpTri> tris;
  tris.reserve(size_t(n - 1) * (n - 1) * subdiv * subdiv * 2);
  for (const int j : IndexRange(n - 1)) {
    for (const int i : IndexRange(n - 1)) {
      for (const int sy : IndexRange(subdiv)) {
        for (const int sx : IndexRange(subdiv)) {
          const float2 g00(float(i * subdiv + sx) / float(subdiv),
                           float(j * subdiv + sy) / float(subdiv));
          const float2 g10(g00.x + 1.0f / float(subdiv), g00.y);
          const float2 g01(g00.x, g00.y + 1.0f / float(subdiv));
          const float2 g11(g00.x + 1.0f / float(subdiv), g00.y + 1.0f / float(subdiv));
          WarpTri a;
          a.tgt[0] = eval_tgt(g00);
          a.tgt[1] = eval_tgt(g10);
          a.tgt[2] = eval_tgt(g11);
          a.src[0] = eval_src(g00);
          a.src[1] = eval_src(g10);
          a.src[2] = eval_src(g11);
          tris.append(a);
          WarpTri b;
          b.tgt[0] = eval_tgt(g00);
          b.tgt[1] = eval_tgt(g11);
          b.tgt[2] = eval_tgt(g01);
          b.src[0] = eval_src(g00);
          b.src[1] = eval_src(g11);
          b.src[2] = eval_src(g01);
          tris.append(b);
        }
      }
    }
  }

  /* A tangent-space normal map is expressed in the UV frame: every triangle's local src -> tgt
   * map (`tgt = L * src`) gives the rotation its stored vectors must follow. */
  bool is_normal_map = (&ima == state.ima) &&
                       state.primary_channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  for (const ImageSelectWarp3DState::WarpExtraTarget &extra : state.extra_targets) {
    if (extra.ima == &ima) {
      is_normal_map = extra.channel == PAINT_MATERIAL_CHANNEL_NORMAL;
    }
  }
  if (is_normal_map) {
    for (WarpTri &tri : tris) {
      float2x2 src_edges;
      src_edges[0] = tri.src[1] - tri.src[0];
      src_edges[1] = tri.src[2] - tri.src[0];
      float2x2 tgt_edges;
      tgt_edges[0] = tri.tgt[1] - tri.tgt[0];
      tgt_edges[1] = tri.tgt[2] - tri.tgt[0];
      bool invert_ok = false;
      const float2x2 src_inv = math::invert(src_edges, invert_ok);
      if (invert_ok) {
        tri.normal_rotation = image_select_normal_rotation(tgt_edges * src_inv);
      }
    }
  }

  ImageUndoStep *us_open = image_select_undo_session_step_get();

  ImageUser tile_iuser = iuser;
  tile_iuser.tile = frag.geom.tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&ima, &tile_iuser, &lock);
  if (ibuf == nullptr) {
    return;
  }
  if (us_open != nullptr) {
    ED_image_undo_push(&ima, ibuf, &tile_iuser, us_open);
    ED_image_undo_capture_selection_mask(&ima, frag.geom.tile_number);
  }

  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
  const float *fmask = frag.pixels.fragment_mask_ibuf ? frag.pixels.fragment_mask_ibuf->float_data() :
                                                        nullptr;
  const ImBuf *blend_mask = frag.edge_policy.use_outward_feather ?
                                frag.preview.fragment_blend_mask_ibuf :
                                nullptr;
  Array<bool> dest_covered(size_t(fw) * size_t(fh), false);

  const auto sample_fragment = [&](const float2 &f, float4 &r_color, float &r_mask) {
    r_mask = 1.0f;
    if (f.x < 0.0f || f.y < 0.0f || f.x >= float(fw) || f.y >= float(fh)) {
      r_mask = 0.0f;
      return false;
    }
    if (blend_mask != nullptr) {
      r_mask = image_select_sample_mask_bilinear(blend_mask, f.x, f.y);
    }
    else if (fmask != nullptr) {
      if (fmask[int(f.y) * fw + int(f.x)] <= SELECTION_MASK_THRESHOLD) {
        r_mask = 0.0f;
        return false;
      }
    }
    if (r_mask <= 0.001f) {
      return false;
    }
    return image_select_fragment_sample_bilinear(frag,
                                                 f,
                                                 ibuf->channels ? ibuf->channels : 4,
                                                 r_color);
  };

  const auto write_dest = [&](const float2 &q, const float4 &color, const float weight) {
    /* q is capture-local px (y-up) -> global UV -> tile pixel. */
    const float2 uv = capture_px_to_uv(q);
    const float2 tile_px = (uv - tile_uv) * tile_size;
    const int px = int(std::floor(tile_px.x));
    const int py = int(std::floor(tile_px.y));
    if (px < 0 || py < 0 || px >= ibuf->x || py >= ibuf->y) {
      return;
    }
    const int64_t idx = int64_t(py) * ibuf->x + px;
    if (ibuf->float_data() != nullptr) {
      const int channels = ibuf->channels ? ibuf->channels : 4;
      float *dst = ibuf->float_data_for_write() + idx * channels;
      for (int c = 0; c < channels; c++) {
        dst[c] = (1.0f - weight) * dst[c] + weight * color[c];
      }
    }
    else {
      uint8_t *dst = ibuf->byte_data_for_write() + idx * 4;
      for (int c = 0; c < 4; c++) {
        dst[c] = uint8_t(std::clamp((1.0f - weight) * float(dst[c]) +
                                        weight * color[c] * 255.0f + 0.5f,
                                    0.0f,
                                    255.0f));
      }
    }
  };

  /* Deformed content pass. */
  for (const WarpTri &tri : tris) {
    float2 tmin(FLT_MAX, FLT_MAX);
    float2 tmax(-FLT_MAX, -FLT_MAX);
    for (const int k : IndexRange(3)) {
      tmin = math::min(tmin, tri.tgt[k]);
      tmax = math::max(tmax, tri.tgt[k]);
    }
    const int x0 = std::max(0, int(std::floor(tmin.x)));
    const int y0 = std::max(0, int(std::floor(tmin.y)));
    const int x1 = std::min(fw - 1, int(std::ceil(tmax.x)));
    const int y1 = std::min(fh - 1, int(std::ceil(tmax.y)));
    const float2 e0 = tri.tgt[1] - tri.tgt[0];
    const float2 e1 = tri.tgt[2] - tri.tgt[0];
    const float denom = e0.x * e1.y - e0.y * e1.x;
    if (std::abs(denom) < 1e-12f) {
      continue;
    }
    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        const float2 p = float2(float(x) + 0.5f, float(y) + 0.5f);
        const float2 d = p - tri.tgt[0];
        const float u = (d.x * e1.y - d.y * e1.x) / denom;
        const float v = (e0.x * d.y - e0.y * d.x) / denom;
        if (u < 0.0f || v < 0.0f || u + v > 1.0f) {
          continue;
        }
        const float2 src = tri.src[0] + (tri.src[1] - tri.src[0]) * u +
                           (tri.src[2] - tri.src[0]) * v;
        float4 color(0.0f, 0.0f, 0.0f, 1.0f);
        float weight = 0.0f;
        if (!sample_fragment(src, color, weight)) {
          continue;
        }
        if (is_normal_map) {
          image_select_normal_color_rotate(color, tri.normal_rotation);
        }
        dest_covered[size_t(y) * fw + x] = true;
        write_dest(p, color, weight);
      }
    }
  }

  /* Restore pass: original-selected pixels the deformed content does not reach keep their
   * original content (and mask value). */
  if (fmask != nullptr && frag.pixels.fragment_ibuf != nullptr) {
    for (int y = 0; y < fh; y++) {
      for (int x = 0; x < fw; x++) {
        if (dest_covered[size_t(y) * fw + x]) {
          continue;
        }
        if (fmask[y * fw + x] <= SELECTION_MASK_THRESHOLD) {
          continue;
        }
        const float2 q(float(x) + 0.5f, float(y) + 0.5f);
        float4 color(0.0f, 0.0f, 0.0f, 1.0f);
        float weight = 0.0f;
        if (sample_fragment(q, color, weight)) {
          write_dest(q, color, 1.0f);
        }
      }
    }
  }

  /* Destination selection mask: 1 inside deformed content (via the warped source mask), the
   * original value elsewhere inside the capture. */
  ImBuf *mask = BKE_image_paint_selection_mask_lookup(&ima, frag.geom.tile_number);
  if (mask != nullptr) {
    float *mdata = mask->float_data_for_write();
    if (mdata != nullptr) {
      for (int y = 0; y < fh; y++) {
        for (int x = 0; x < fw; x++) {
          const float2 q(float(x) + 0.5f, float(y) + 0.5f);
          const float2 uv = capture_px_to_uv(q);
          const float2 tile_px = (uv - tile_uv) * tile_size;
          const int px = int(std::floor(tile_px.x));
          const int py = int(std::floor(tile_px.y));
          if (px < 0 || py < 0 || px >= mask->x || py >= mask->y) {
            continue;
          }
          if (dest_covered[size_t(y) * fw + x]) {
            mdata[py * mask->x + px] = 1.0f;
          }
          else if (fmask != nullptr && fmask[y * fw + x] > SELECTION_MASK_THRESHOLD) {
            mdata[py * mask->x + px] = fmask[y * fw + x];
          }
        }
      }
    }
  }

  rcti dirty;
  BLI_rcti_init(&dirty,
                std::max(0, frag.geom.origin_px.x),
                std::min(ibuf->x, frag.geom.origin_px.x + fw),
                std::max(0, frag.geom.origin_px.y),
                std::min(ibuf->y, frag.geom.origin_px.y + fh));
  BKE_image_partial_update_mark_region(&ima, BKE_image_get_tile(&ima, frag.geom.tile_number),
                                       ibuf, &dirty);
  BKE_image_release_ibuf(&ima, ibuf, lock);

  BKE_image_mark_dirty(&ima, ibuf);
  /* Cached GPU textures are not refreshed by the partial-update marks alone; reload them from the
   * modified buffer like the Image Editor's warp commit does. */
  BKE_image_free_gputextures(&ima);
  DEG_id_tag_update(&ima.id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, &ima);
}

void ImageSelectWarp3DState::commit(bContext *C)
{
  warp3d_free_draw_handle(*this);
  WM_cursor_modal_restore(CTX_wm_window(C));

  /* The committable targets, primary first: a Material canvas warps one canvas per PBR map from
   * the shared grid, so the whole commit lands in ONE image undo step and a single Ctrl+Z
   * restores every canvas together (see #image_select_view3d_commit_targets_with_undo). */
  Vector<ImagePaintSelectionTarget> commit_targets;
  Vector<Span<SelectionTileFragment>> commit_fragments;
  if (this->ima != nullptr && this->has_fragment &&
      image_select_view3d_image_is_alive(*this, this->ima))
  {
    commit_targets.append(ImagePaintSelectionTarget{this->ima, this->iuser});
    commit_fragments.append(Span<SelectionTileFragment>(&this->fragment, 1));
  }
  for (WarpExtraTarget &extra : this->extra_targets) {
    if (extra.ima != nullptr && extra.has_fragment &&
        image_select_view3d_image_is_alive(*this, extra.ima))
    {
      commit_targets.append(ImagePaintSelectionTarget{extra.ima, extra.iuser});
      commit_fragments.append(Span<SelectionTileFragment>(&extra.fragment, 1));
    }
  }
  image_select_view3d_commit_targets_with_undo(
      C,
      "Warp Selection",
      commit_targets,
      commit_fragments,
      [&](Image &ima, const ImageUser &iuser, Span<SelectionTileFragment> fragments) {
        for (const SelectionTileFragment &frag : fragments) {
          warp3d_write_final_target(C, *this, ima, iuser, frag);
        }
      });
}

/** Cancel one target's lifted fragment: restore its source region. */
static void warp3d_target_cancel(bContext *C,
                                 ImageSelectWarp3DState &state,
                                 Image *ima,
                                 const ImageUser &iuser,
                                 SelectionTileFragment &fragment,
                                 const bool has_fragment)
{
  if (ima == nullptr || !has_fragment || !image_select_view3d_image_is_alive(state, ima)) {
    return;
  }
  image_select_fragment_restore_source(C, ima, iuser, Span(&fragment, 1));
  Vector<ImagePaintSelectionTarget> single = {ImagePaintSelectionTarget{ima, iuser}};
  image_paint_selection_targets_update(C, single);
}

void ImageSelectWarp3DState::cancel(bContext *C)
{
  warp3d_free_draw_handle(*this);
  if (C != nullptr) {
    WM_cursor_modal_restore(CTX_wm_window(C));
  }
  warp3d_target_cancel(
      C, *this, this->ima, this->iuser, this->fragment, this->has_fragment);
  for (ImageSelectWarp3DState::WarpExtraTarget &target : this->extra_targets) {
    warp3d_target_cancel(
        C, *this, target.ima, target.iuser, target.fragment, target.has_fragment);
  }
}

bool ImageSelectWarp3DState::undo_step()
{
  if (this->drag_history.is_empty()) {
    return false;
  }
  this->tgt_pts = this->drag_history.pop_last();
  if (this->owner_region != nullptr) {
    ED_region_tag_redraw(this->owner_region);
  }
  return true;
}

bool ImageSelectWarp3DState::cursor_over_fragment(const bContext * /*C*/,
                                                  const int2 & /*mval*/) const
{
  /* The warp owns its grid handles; gestures do not delegate into it. */
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

static ImageSelectWarp3DState *warp3d_state_get()
{
  return image_select_view3d_session_get<ImageSelectWarp3DState, PaintSelectView3DTool::Warp>();
}

static wmOperatorStatus warp3d_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return OPERATOR_PASS_THROUGH;
  }
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr) {
    return OPERATOR_PASS_THROUGH;
  }

  /* Drag path: pick a grid point of a live warp session of this Viewport. */
  if (ImageSelectWarp3DState *floating = warp3d_state_get()) {
    if (floating->owner_v3d != CTX_wm_view3d(C) || floating->owner_region != region) {
      return OPERATOR_PASS_THROUGH;
    }
    floating->drag_point = warp3d_pick_point(*floating, int2(event->mval[0], event->mval[1]));
    if (floating->drag_point < 0) {
      return OPERATOR_PASS_THROUGH;
    }
    floating->drag_history.append(floating->tgt_pts);
    floating->is_dragging = true;
    WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
    WM_event_add_modal_handler(C, op);
    ED_region_tag_redraw(region);
    return OPERATOR_RUNNING_MODAL;
  }

  /* Another tool's session may still float in this Viewport: settle it before this tool lifts its
   * own fragments (the session slot must be empty, see #image_select_view3d_session_set). */
  image_select_view3d_session_end(C, true);
  if (!image_paint_selection_view3d_poll(C)) {
    return OPERATOR_PASS_THROUGH;
  }
  auto *state = MEM_new<ImageSelectWarp3DState>(__func__);
  if (!image_select_view3d_anchor_from_mval(C, int2(event->mval[0], event->mval[1]), state->anchor)) {
    MEM_delete(state);
    BKE_report(op->reports, RPT_WARNING, "Selection: no surface under the cursor");
    return OPERATOR_CANCELLED;
  }
  const Scene *scene = CTX_data_scene(C);
  state->grid_size = std::clamp(int(scene->toolsettings->imapaint.warp_grid_size), 2, 10);
  state->interp = int(scene->toolsettings->imapaint.warp_interpolation);

  /* Lift the fragment with a selection: the first target drives the shared grid, the remaining
   * Material targets get their own fragments warped with the same grid so all canvases stay in
   * sync (single-fragment warp per canvas, 2D parity). */
  Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C, *ob);
  for (const ImagePaintSelectionTarget &target : targets) {
    SelectionTileFragment extracted;
    if (!warp3d_extract_first_tile(op, target.image, target.iuser, &extracted)) {
      continue;
    }
    if (!state->has_fragment) {
      state->ima = target.image;
      state->iuser = target.iuser;
      state->fragment = extracted;
      state->has_fragment = true;
      state->primary_channel = target.channel;
      image_select_fragment_lift_source(C, target.image, target.iuser, Span(&state->fragment, 1));
    }
    else {
      ImageSelectWarp3DState::WarpExtraTarget extra;
      extra.ima = target.image;
      extra.iuser = target.iuser;
      extra.channel = target.channel;
      extra.fragment = extracted;
      extra.has_fragment = true;
      image_select_fragment_lift_source(C, target.image, target.iuser, Span(&extra.fragment, 1));
      state->extra_targets.append(std::move(extra));
    }
    state->borrowed_images.append(target.image);
  }
  if (!state->has_fragment) {
    MEM_delete(state);
    BKE_report(op->reports, RPT_WARNING, "Selection is empty");
    return OPERATOR_CANCELLED;
  }
  if (state->anchor.object != nullptr) {
    state->surface_map.ensure(*state->anchor.object, state->anchor.uv_map_name());
  }
  warp3d_init_grid(*state);
  if (region->runtime->type != nullptr) {
    state->owner_region_type = region->runtime->type;
    /* POST_VIEW first (the fragment pixels lie under the grid drawn POST_PIXEL). */
    state->draw_handle_texture = ED_region_draw_cb_activate(
        state->owner_region_type, warp3d_draw_texture, state, REGION_DRAW_POST_VIEW);
    state->draw_handle = ED_region_draw_cb_activate(
        state->owner_region_type, warp3d_draw, state, REGION_DRAW_POST_PIXEL);
  }
  image_select_view3d_session_set(C, CTX_wm_view3d(C), state);
  state->drag_history.append(state->tgt_pts);

  /* LMB straight onto a point starts a drag immediately. #screen_pts are filled here (not just by
   * the draw callback): without the projection the pick below always missed -- the grid was not
   * drawn yet -- and the first click never started a drag. */
  warp3d_refresh_screen(*state, *region);
  state->drag_point = warp3d_pick_point(*state, int2(event->mval[0], event->mval[1]));
  state->is_dragging = state->drag_point >= 0;
  if (state->is_dragging) {
    WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
  }
  WM_event_add_modal_handler(C, op);
  ED_region_tag_redraw(region);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus warp3d_modal(bContext *C, wmOperator * /*op*/, const wmEvent *event)
{
  ImageSelectWarp3DState *state = warp3d_state_get();
  ARegion *region = CTX_wm_region(C);
  if (state == nullptr || region == nullptr || state->owner_region != region) {
    if (state != nullptr) {
      state->is_dragging = false;
      WM_cursor_modal_restore(CTX_wm_window(C));
    }
    return OPERATOR_FINISHED;
  }

  if (state->is_dragging) {
    if (event->type == MOUSEMOVE) {
      float2 plane_now;
      if (image_select_view3d_mval_to_plane_px(state->anchor,
                                               *region,
                                               int2(event->mval[0], event->mval[1]),
                                               plane_now) &&
          state->anchor.jacobian_valid)
      {
        const float2 uv = state->anchor.anchor_uv +
                          image_select_view3d_plane_px_to_uv_delta(state->anchor, plane_now);
        state->tgt_pts[state->drag_point] = state->uv_to_capture_px(uv);
        ED_region_tag_redraw(region);
      }
      return OPERATOR_RUNNING_MODAL;
    }
    if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
      state->is_dragging = false;
      state->drag_point = -1;
      WM_cursor_modal_restore(CTX_wm_window(C));
      ED_region_tag_redraw(region);
      return OPERATOR_FINISHED;
    }
    if (event->type == EVT_ESCKEY && event->val == KM_PRESS) {
      if (!state->drag_history.is_empty()) {
        state->tgt_pts = state->drag_history.pop_last();
      }
      state->is_dragging = false;
      state->drag_point = -1;
      WM_cursor_modal_restore(CTX_wm_window(C));
      ED_region_tag_redraw(region);
      return OPERATOR_FINISHED;
    }
    return OPERATOR_RUNNING_MODAL;
  }

  if (ELEM(event->type,
           MIDDLEMOUSE,
           WHEELUPMOUSE,
           WHEELDOWNMOUSE,
           WHEELINMOUSE,
           WHEELOUTMOUSE,
           MOUSEPAN,
           MOUSEZOOM,
           MOUSEROTATE,
           NDOF_MOTION))
  {
    return OPERATOR_PASS_THROUGH;
  }
  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case IMAGE_SELECT_FLOATING_MODAL_CONFIRM:
        image_select_view3d_session_end(C, true);
        return OPERATOR_FINISHED;
      case IMAGE_SELECT_FLOATING_MODAL_CANCEL:
        image_select_view3d_session_end(C, false);
        return OPERATOR_FINISHED;
      case IMAGE_SELECT_FLOATING_MODAL_UNDO_STEP: {
        if (!state->drag_history.is_empty()) {
          state->tgt_pts = state->drag_history.pop_last();
          ED_region_tag_redraw(region);
        }
        else {
          image_select_view3d_session_end(C, false);
          return OPERATOR_FINISHED;
        }
        break;
      }
      default:
        break;
    }
    return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_PASS_THROUGH;
}

void PAINT_OT_image_select_view3d_warp(wmOperatorType *ot)
{
  ot->name = "Warp Selection";
  ot->idname = "PAINT_OT_image_select_view3d_warp";
  ot->description = "Warp the selection with a control grid drawn on the surface";
  ot->invoke = warp3d_invoke;
  ot->modal = warp3d_modal;
  ot->poll = image_paint_selection_view3d_tool_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

}  // namespace blender::ed::sculpt_paint
