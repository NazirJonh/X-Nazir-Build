/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Gizmo group for floating Image Paint selection transform (cage2d + anchor).
 */

#include "DNA_space_types.h"
#include "DNA_userdef_types.h"

#include "BKE_context.hh"
#include "BKE_global.hh"

#include "ED_gizmo_library.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "BLI_math_matrix.h"

#include "../../space_image/image_runtime.hh"
#include "paint_image_select_intern.hh"

namespace blender {

struct PaintSelectTransformGizmoGroup {
  wmGizmo *gz_cage = nullptr;
  wmGizmo *gz_anchor = nullptr;
  /** Tracks #WM_gizmomap_get_modal so we can end drag when tweak finishes without a custom_modal release event. */
  bool was_modal_tweak = false;
};

static PaintSelectTransformGizmoGroup *paint_select_transform_gizmo_get(wmGizmoGroup *gzgroup)
{
  return static_cast<PaintSelectTransformGizmoGroup *>(gzgroup->customdata);
}

static wmOperatorStatus paint_select_transform_cage_modal(bContext *C,
                                                           wmGizmo *gz,
                                                           const wmEvent *event,
                                                           eWM_GizmoFlagTweak /*tweak_flag*/)
{
  PaintSelectTransformGizmoGroup *ggd = paint_select_transform_gizmo_get(gz->parent_gzgroup);
  ImageSelectTransformState *state = image_select_transform_state_get(CTX_wm_space_image(C));
  if (!ggd || !state) {
    return OPERATOR_FINISHED;
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return OPERATOR_RUNNING_MODAL;
  }

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    image_select_transform_end_drag(state);
    ED_region_tag_redraw(region);
    return OPERATOR_FINISHED;
  }

  if (event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }

  const ImageSelectTransformHandleType handle =
      image_select_transform_cage_part_to_handle_type(gz->highlight_part);
  if (handle == ImageSelectTransformHandleType::None) {
    return OPERATOR_RUNNING_MODAL;
  }

  if (!image_select_transform_has_active_handle(state)) {
    /* Fallback if invoke_prepare did not run. */
    image_select_transform_begin_drag(state, event, handle);
  }

  image_select_transform_apply_handle(C, state, event, handle, region);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus paint_select_transform_anchor_modal(bContext *C,
                                                              wmGizmo *gz,
                                                              const wmEvent *event,
                                                              eWM_GizmoFlagTweak /*tweak_flag*/)
{
  PaintSelectTransformGizmoGroup *ggd = paint_select_transform_gizmo_get(gz->parent_gzgroup);
  ImageSelectTransformState *state = image_select_transform_state_get(CTX_wm_space_image(C));
  if (!ggd || !state) {
    return OPERATOR_FINISHED;
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return OPERATOR_RUNNING_MODAL;
  }

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    image_select_transform_end_drag(state);
    ED_region_tag_redraw(region);
    return OPERATOR_FINISHED;
  }

  if (event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }

  if (!image_select_transform_has_active_handle(state)) {
    image_select_transform_begin_drag(state, event, ImageSelectTransformHandleType::Anchor);
  }

  image_select_transform_apply_handle(
      C, state, event, ImageSelectTransformHandleType::Anchor, region);
  return OPERATOR_RUNNING_MODAL;
}

static void paint_select_transform_gizmo_invoke_prepare(const bContext *C,
                                                        wmGizmoGroup *gzgroup,
                                                        wmGizmo *gz,
                                                        const wmEvent *event)
{
  PaintSelectTransformGizmoGroup *ggd = paint_select_transform_gizmo_get(gzgroup);
  ImageSelectTransformState *state = image_select_transform_state_get(CTX_wm_space_image(C));
  if (!ggd || !state || !event) {
    return;
  }

  if (gz == ggd->gz_anchor) {
    image_select_transform_begin_drag(state, event, ImageSelectTransformHandleType::Anchor);
  }
  else if (gz == ggd->gz_cage) {
    const ImageSelectTransformHandleType handle =
        image_select_transform_cage_part_to_handle_type(gz->highlight_part);
    if (handle != ImageSelectTransformHandleType::None) {
      image_select_transform_begin_drag(state, event, handle);
    }
  }
}

static bool paint_select_transform_gizmo_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  if ((U.gizmo_flag & USER_GIZMO_DRAW) == 0) {
    return false;
  }

  const SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || sima->mode != SI_MODE_PAINT) {
    return false;
  }

  return image_select_transform_is_floating_in_space(sima);
}

static void paint_select_transform_gizmo_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  const wmGizmoType *gzt_cage = WM_gizmotype_find("GIZMO_GT_cage_2d", true);
  const wmGizmoType *gzt_move3d = WM_gizmotype_find("GIZMO_GT_move_3d", true);

  PaintSelectTransformGizmoGroup *ggd = MEM_new<PaintSelectTransformGizmoGroup>(__func__);
  gzgroup->customdata = ggd;
  gzgroup->customdata_free = [](void *customdata) {
    MEM_delete(static_cast<PaintSelectTransformGizmoGroup *>(customdata));
  };

  /* Anchor before cage so pivot hits win over the interior move region. */
  ggd->gz_anchor = WM_gizmo_new_ptr(gzt_move3d, gzgroup, nullptr);
  WM_gizmo_set_fn_custom_modal(ggd->gz_anchor, paint_select_transform_anchor_modal);
  RNA_enum_set(ggd->gz_anchor->ptr, "draw_style", ED_GIZMO_MOVE_STYLE_RING_2D);
  ggd->gz_anchor->flag |= WM_GIZMO_DRAW_MODAL | WM_GIZMO_DRAW_NO_SCALE;
  float anchor_color[4] = {0.1f, 0.6f, 1.0f, 1.0f};
  float anchor_color_hi[4] = {0.3f, 0.8f, 1.0f, 1.0f};
  WM_gizmo_set_color(ggd->gz_anchor, anchor_color);
  WM_gizmo_set_color_highlight(ggd->gz_anchor, anchor_color_hi);
  WM_gizmo_set_scale(ggd->gz_anchor, 0.15f);

  ggd->gz_cage = WM_gizmo_new_ptr(gzt_cage, gzgroup, nullptr);
  RNA_enum_set(ggd->gz_cage->ptr, "draw_style", ED_GIZMO_CAGE2D_STYLE_BOX_TRANSFORM);
  RNA_enum_set(ggd->gz_cage->ptr,
               "transform",
               ED_GIZMO_CAGE_XFORM_FLAG_TRANSLATE | ED_GIZMO_CAGE_XFORM_FLAG_SCALE |
                   ED_GIZMO_CAGE_XFORM_FLAG_ROTATE);
  /* Corner handles enable corner hit-testing; all-handles draws them permanently;
   * center-handle-plus draws the interior cross as a plus glyph. */
  RNA_enum_set(ggd->gz_cage->ptr,
               "draw_options",
               ED_GIZMO_CAGE_DRAW_FLAG_XFORM_CENTER_HANDLE |
                   ED_GIZMO_CAGE_DRAW_FLAG_XFORM_CENTER_HANDLE_PLUS |
                   ED_GIZMO_CAGE_DRAW_FLAG_CORNER_HANDLES |
                   ED_GIZMO_CAGE_DRAW_FLAG_ALL_HANDLES |
                   ED_GIZMO_CAGE_DRAW_FLAG_XFORM_INTERIOR_TRANSLATE);
  WM_gizmo_set_fn_custom_modal(ggd->gz_cage, paint_select_transform_cage_modal);
  /* Interior-translate cursor depends on modal state, so refresh it on modal transitions. */
  ggd->gz_cage->flag |= WM_GIZMO_REFRESH_CURSOR_ON_MODAL;

  float cage_color[4] = {1.0f, 0.85f, 0.0f, 0.9f};
  float cage_color_hi[4] = {1.0f, 1.0f, 0.3f, 1.0f};
  WM_gizmo_set_color(ggd->gz_cage, cage_color);
  WM_gizmo_set_color_highlight(ggd->gz_cage, cage_color_hi);
  WM_gizmo_set_line_width(ggd->gz_cage, 1.5f);
}

static void paint_select_transform_gizmo_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  PaintSelectTransformGizmoGroup *ggd = paint_select_transform_gizmo_get(gzgroup);
  if (!ggd) {
    return;
  }

  const SpaceImage *sima = CTX_wm_space_image(C);
  const bool show = image_select_transform_is_floating_in_space(sima);

  if (show) {
    ggd->gz_cage->flag &= ~WM_GIZMO_HIDDEN;
    ggd->gz_anchor->flag &= ~WM_GIZMO_HIDDEN;
  }
  else {
    ggd->gz_cage->flag |= WM_GIZMO_HIDDEN;
    ggd->gz_anchor->flag |= WM_GIZMO_HIDDEN;
  }

  image_select_transform_gizmo_refresh_tweak(
      C, ggd->gz_cage, ggd->gz_anchor, &ggd->was_modal_tweak);
}

static void paint_select_transform_gizmo_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  PaintSelectTransformGizmoGroup *ggd = paint_select_transform_gizmo_get(gzgroup);
  if (!ggd) {
    return;
  }

  SpaceImage *sima = CTX_wm_space_image(C);
  const ImageSelectTransformState *state = image_select_transform_state_get(sima);
  if (!state) {
    return;
  }

  ImageSelectTransformGizmoMatrices mats;
  if (!image_select_transform_calc_gizmo_matrices(C, state, &mats)) {
    return;
  }

  copy_m4_m4(ggd->gz_cage->matrix_space, mats.matrix_space);
  copy_m4_m4(ggd->gz_cage->matrix_basis, mats.matrix_basis);
  copy_m4_m4(ggd->gz_cage->matrix_offset, mats.matrix_offset);
  /* matrix_offset[3] already includes center/pivot layout for rotation (see calc_gizmo_matrices). */

  unit_m4(ggd->gz_anchor->matrix_space);
  unit_m4(ggd->gz_anchor->matrix_basis);
  unit_m4(ggd->gz_anchor->matrix_offset);
  WM_gizmo_set_matrix_location(ggd->gz_anchor, mats.anchor_screen);
}

void ED_image_paint_select_transform_gizmo_setup(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Paint Select Transform";
  gzgt->idname = "IMAGE_GGT_paint_select_transform";

  gzgt->flag = WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL;

  gzgt->poll = paint_select_transform_gizmo_poll;
  gzgt->setup = paint_select_transform_gizmo_setup;
  gzgt->invoke_prepare = paint_select_transform_gizmo_invoke_prepare;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = paint_select_transform_gizmo_refresh;
  gzgt->draw_prepare = paint_select_transform_gizmo_draw_prepare;
}

} /* namespace blender */
