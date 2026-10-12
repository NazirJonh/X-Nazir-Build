/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include <climits>
#include <cstdio>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_armature_types.h"
#include "DNA_collection_types.h"
#include "DNA_constraint_types.h"
#include "DNA_gpencil_legacy_types.h"
#include "DNA_key_types.h"
#include "DNA_layer_types.h"
#include "DNA_light_types.h"
#include "DNA_lightprobe_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_text_types.h"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_utildefines.h"

#include "BLF_api.hh"
#include "BLT_translation.hh"

#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_deform.hh"
#include "BKE_global.hh"
#include "BKE_gpencil_legacy.h"
#include "BKE_grease_pencil.hh"
#include "BKE_idtype.hh"
#include "BKE_key.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_override.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_main_namemap.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_particle.h"
#include "BKE_preview_image.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "ANIM_armature.hh"
#include "ANIM_bone_collections.hh"
#include "ANIM_keyframing.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_armature.hh"
#include "ED_fileselect.hh"
#include "ED_id_management.hh"
#include "ED_outliner.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_types.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_state.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "UI_interface.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "../outliner_intern.hh"
#include "../tree/tree_element.hh"
#include "../tree/tree_element_grease_pencil_node.hh"
#include "../tree/tree_element_id.hh"
#include "../tree/tree_element_overrides.hh"
#include "../tree/tree_element_rna.hh"
#include "../tree/tree_element_seq.hh"
#include "../tree/tree_iterator.hh"
#include "outliner_stack_source.hh"

namespace blender {

namespace ed::outliner {

/**
 * The empty-slot placeholder: the source's icon at preview size with a faint frame, standing
 * for a slot whose data exists but has nothing to show in its thumbnail yet.
 */
void stack_preview_empty_draw(const rctf &preview_rect, const int icon, const float alpha_fac)
{
  ui::icon_draw_ex(preview_rect.xmin,
                   preview_rect.ymin,
                   icon,
                   UI_INV_SCALE_FAC,
                   alpha_fac,
                   0.0f,
                   nullptr,
                   false,
                   nullptr,
                   false,
                   OUTLINER_STACK_PREVIEW_SCALE);

  float preview_border[4];
  ui::theme::get_color_blend_4f(TH_TEXT, TH_BACK, 0.9f, preview_border);
  preview_border[3] *= alpha_fac;
  const float margin = OUTLINER_STACK_PREVIEW_FRAME_MARGIN * UI_SCALE_FAC;
  rctf frame{};
  BLI_rctf_init(&frame,
                preview_rect.xmin - margin,
                preview_rect.xmax + margin,
                preview_rect.ymin - margin,
                preview_rect.ymax + margin);
  draw_roundbox_corner_set(ui::CNR_ALL);
  ui::draw_roundbox_aa(&frame, false, UI_UNIT_Y / 5.0f, preview_border);
  GPU_blend(GPU_BLEND_ALPHA); /* Round-box disables. */
}

/**
 * The flat-colour swatch of a row that *is* a colour -- a Fill layer: the colour itself at preview
 * size, framed the way the empty-texture placeholder is.
 */
void stack_preview_color_draw(const rctf &preview_rect,
                              const float color[4],
                              const float alpha_fac)
{
  /* The slot carries scene linear; the UI draws display colours. */
  float swatch[4];
  linearrgb_to_srgb_v4(swatch, color);
  swatch[3] *= alpha_fac;

  GPU_blend(GPU_BLEND_ALPHA);
  draw_roundbox_corner_set(ui::CNR_ALL);
  ui::draw_roundbox_aa(&preview_rect, true, UI_UNIT_Y / 5.0f, swatch);
  GPU_blend(GPU_BLEND_ALPHA); /* Round-box disables. */

  float swatch_border[4];
  ui::theme::get_color_blend_4f(TH_TEXT, TH_BACK, 0.9f, swatch_border);
  swatch_border[3] *= alpha_fac;
  const float margin = OUTLINER_STACK_PREVIEW_FRAME_MARGIN * UI_SCALE_FAC;
  rctf frame{};
  BLI_rctf_init(&frame,
                preview_rect.xmin - margin,
                preview_rect.xmax + margin,
                preview_rect.ymin - margin,
                preview_rect.ymax + margin);
  draw_roundbox_corner_set(ui::CNR_ALL);
  ui::draw_roundbox_aa(&frame, false, UI_UNIT_Y / 5.0f, swatch_border);
  GPU_blend(GPU_BLEND_ALPHA); /* Round-box disables. */
}

/**
 * Width a Stack Layers row's preview slots occupy, from the row's own icon ahead of the first slot
 * to the frame around the last. The draw advances its content by it, and the rename field starts
 * after it, so both read one number rather than rebuilding the sum from the slots by hand.
 *
 * `leading_icon` is true for a row that keeps its own icon ahead of the slots -- a folder with a
 * mask. `num_previews` may be zero for a row that only reserves a placeholder slot.
 */
float outliner_stack_preview_row_width(const bool leading_icon, const int num_previews)
{
  const int slots = std::max(1, num_previews);
  const float leading_icon_width = leading_icon ? UI_UNIT_X + 4.0f * (UI_UNIT_X / 20.0f) : 0.0f;
  const float preview_gap = UI_UNIT_X * 0.25f;
  return leading_icon_width + slots * outliner_stack_preview_size() + (slots - 1) * preview_gap +
         2.0f * OUTLINER_STACK_PREVIEW_FRAME_MARGIN * UI_SCALE_FAC;
}

/**
 * Where the one-unit-tall content of a row whose bottom is \a row_bottom sits.
 *
 * A Stack Layers row is two units tall with `SO_SL_SMALL_ROWS` off, and its content -- name, icon,
 * toggles, columns -- is centered in that: a name pinned to one edge of a tall row reads as
 * belonging to the row next to it.
 */
int stack_row_content_offset(const int row_bottom, const int row_height)
{
  return row_bottom + (row_height - UI_UNIT_Y) / 2;
}

/**
 * The same line, for the passes that draw their buttons from `te->ys`.
 *
 * They run after the tree has been laid out, so they have the row's bottom rather than the running
 * position #outliner_draw_tree_element works with.
 */
int stack_row_content_y(const SpaceOutliner &space_outliner, const TreeElement &te)
{
  return stack_row_content_offset(te.ys, outliner_tree_element_height(space_outliner, te));
}

/**
 * The x a child of a Stack Layers row is drawn at. A correction or mask row hangs off its layer
 * rather than nesting under it, so it keeps the layer's own indent: the one column the hierarchy
 * would add only left empty space beside its visibility toggle.
 */
int stack_child_start_x(const SpaceOutliner &space_outliner,
                        const TreeElement &child,
                        const int parent_startx)
{
  const TreeStoreElem *tselem = TREESTORE(&child);
  if (tselem->type == TSE_STACK_LAYER) {
    const StackRow *row = outliner_stack_row_find(space_outliner, tselem->nr);
    if (row != nullptr && !row->parent_section_id.empty()) {
      /* A row under the grouping's mask section lines up under the parent's mask preview: its
       * toggle, one unit in from its start, then sits right below that button. Without previews
       * there is nothing to line up with. */
      const StringRefNull aux_section = stack_aux_section_id(space_outliner);
      if (!aux_section.is_empty() && row->parent_section_id == aux_section &&
          child.parent != nullptr && (space_outliner.stack_layers_flag & SO_SL_SMALL_ROWS) == 0)
      {
        const TreeStoreElem *parent_tselem = TREESTORE(child.parent);
        const StackRow *parent_row = (parent_tselem->type == TSE_STACK_LAYER) ?
                                         outliner_stack_row_find(space_outliner,
                                                                 parent_tselem->nr) :
                                         nullptr;
        if (parent_row != nullptr && !parent_row->compact) {
          for (const int slot_index : parent_row->preview_slots.index_range()) {
            if (parent_row->preview_slots[slot_index].section_id == aux_section) {
              const rctf mask_rect = outliner_stack_row_preview_rect(
                  *parent_row, float(parent_startx), 0.0f, slot_index);
              return int(mask_rect.xmin) - int(UI_UNIT_X);
            }
          }
        }
      }
      return parent_startx;
    }
  }
  return parent_startx + int(UI_UNIT_X);
}

/**
 * Whether a Stack Layers #TSE_STACK_LAYER row should read as selected -- either because its own
 * row is, or because one of the #TSE_STACK_ITEM rows it holds is. Switching which sub-row is open
 * is a detail of working inside the row, not a different selection, so the row's own highlight
 * should not blink out when the click lands on one of its sub-rows instead of on the row itself.
 */
bool stack_layer_row_selected(const TreeElement &te)
{
  if (TREESTORE(&te)->flag & TSE_SELECTED) {
    return true;
  }
  for (const TreeElement &child : te.subtree) {
    if (TREESTORE(&child)->type == TSE_STACK_ITEM && (TREESTORE(&child)->flag & TSE_SELECTED)) {
      return true;
    }
  }
  return false;
}

bool stack_layer_row_active(const TreeElement &te)
{
  if (TREESTORE(&te)->flag & TSE_ACTIVE) {
    return true;
  }
  for (const TreeElement &child : te.subtree) {
    if (TREESTORE(&child)->type == TSE_STACK_ITEM && (TREESTORE(&child)->flag & TSE_ACTIVE)) {
      return true;
    }
  }
  return false;
}

/**
 * Whether any row in the Stack Layers tree has the TSE_ACTIVE flag set.
 *
 * Stack Layers rows can be lit two ways: by the source (row_is_active) and by the tree
 * (TSE_ACTIVE). When a folder is created, it gets TSE_ACTIVE, but row_is_active keeps
 * returning true for the texture layer that was active before -- leading to two rows
 * lighting up at once. TSE_ACTIVE is the single source of truth for which row is actually
 * active: if it exists anywhere in the tree, only TSE_ACTIVE rows light up, and
 * row_is_active is ignored. This check is done once before drawing, rather than on each
 * row, to avoid repeated tree traversals.
 */
bool stack_tree_has_tse_active(const SpaceOutliner &space_outliner)
{
  bool found = false;
  tree_iterator::all_open(space_outliner, [&](const TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (ELEM(tselem->type, TSE_STACK_LAYER, TSE_STACK_ITEM) && (tselem->flag & TSE_ACTIVE)) {
      found = true;
    }
  });
  return found;
}

}  // namespace ed::outliner
}  // namespace blender
