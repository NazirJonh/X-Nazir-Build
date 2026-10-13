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
 * How far the visibility toggle is kept from the edges of the column it sits in.
 *
 * The toggle is shifted two pixels toward the hierarchy: its left inset keeps it clear of the
 * region border while its right edge meets the beginning of the tree.
 */
static float stack_visibility_column_left_inset()
{
  return 8.0f * UI_SCALE_FAC;
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

bool stack_row_has_inline_toggle(const StackRow *row)
{
  return row != nullptr && row->compact && row->supported && !row->is_bare_base;
}

/**
 * The per-row toggles that sit right after a stack row's name.
 *
 * Drawn as a pass of its own rather than from #outliner_draw_tree_element, so that the buttons all
 * land in the block at once and the tree drawing stays about drawing the tree. The name has
 * already been laid out by then, and `te->xend` is where it ended.
 */
void outliner_draw_stack_row_icons(ui::Block *block,
                                   ARegion *region,
                                   SpaceOutliner *space_outliner,
                                   const bContext & /*C*/,
                                   const TreeViewContext &tvc)
{
  const StackReadContext ctx = {tvc.bmain, tvc.scene, tvc.view_layer};
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return;
  }
  const StackSource &source = *stack_source_for_space(*space_outliner);
  const bool can_toggle = source.can_edit(*owner);
  const bool visibility_left = (space_outliner->stack_layers_flag & SO_SL_VISIBILITY_RIGHT) == 0;

  /* The toggle sits in its own column at the right edge, after the value and mode ones --
   * unless it was moved to the dedicated left column instead, in which case this is only where
   * the "row not supported" badge sits. No margin towards the mode column:
   * #outliner_right_columns_width reserves the columns flush against each other, and an extra
   * offset here would push the toggle into the mode button. */
  const int icon_columns = visibility_left ? 0 : source.column_layout().icon_columns;
  const int icons_x = int(region->v2d.cur.xmax) - icon_columns * UI_UNIT_X - V2D_SCROLL_WIDTH;
  /* The column immediately before the tree. With the left-side toggle enabled, the whole tree is
   * shifted right by one unit to reserve this column for its buttons. */
  const TreeElement *root = static_cast<const TreeElement *>(space_outliner->runtime->tree.first);
  const int first_column_x = (root != nullptr) ? root->xs - UI_UNIT_X : 0;
  const float column_left_inset = stack_visibility_column_left_inset();

  tree_iterator::all_open(*space_outliner, [&](TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type != TSE_STACK_LAYER ||
        !outliner_is_element_in_view(*space_outliner, te, &region->v2d))
    {
      return;
    }
    const StackRow *row = outliner_stack_row_find(*space_outliner, tselem->nr);
    if (row == nullptr) {
      return;
    }
    const int content_y = stack_row_content_y(*space_outliner, *te);
    if (!row->supported) {
      /* A row the source could not represent is listed anyway, so the user can see that something
       * is there; the badge is where the reason lives. */
      uiDefIconBut(block,
                   ui::ButtonType::Label,
                   ICON_ERROR,
                   icons_x,
                   content_y,
                   UI_UNIT_X,
                   UI_UNIT_Y,
                   nullptr,
                   0.0,
                   0.0,
                   row->unsupported_reason == nullptr ? TIP_("This layer is not supported") :
                                                        TIP_(row->unsupported_reason));
      return;
    }
    /* A bare base has no Mix node to mute, so there is nothing to turn off. */
    if (!can_toggle || row->is_bare_base) {
      return;
    }

    /* Before the name or in the columns, whichever the user asked for. On the left it goes in the
     * column reserved before the tree, so the toggles of nested rows line up with the ones above
     * them without overlapping the hierarchy. */
    /* A correction's toggle is in the row, in the slot #outliner_draw_tree_element keeps free
     * between the expand arrow and the icon. */
    const bool inline_toggle = stack_row_has_inline_toggle(row);
    ui::Button *but = uiDefIconButO(block,
                                    ui::ButtonType::ButToggle,
                                    "OUTLINER_OT_stack_layer_visibility_toggle",
                                    wm::OpCallContext::ExecDefault,
                                    row->enabled ? ICON_HIDE_OFF : ICON_HIDE_ON,
                                    /* Shifted slightly toward the hierarchy within the dedicated
                                     * column, with its right edge meeting the tree. */
                                    inline_toggle   ? te->xs + int(UI_UNIT_X) :
                                    visibility_left ? first_column_x + int(column_left_inset) :
                                                      icons_x,
                                    content_y,
                                    inline_toggle   ? int(UI_UNIT_X) :
                                    visibility_left ? UI_UNIT_X - int(column_left_inset) :
                                                      UI_UNIT_X,
                                    UI_UNIT_Y,
                                    row->enabled ? TIP_("Hide this layer") :
                                                   TIP_("Show this layer"));
    RNA_int_set(ui::button_operator_ptr_ensure(but), "ordinal", row->ordinal);
    /* The state the button shows is the row's own, captured here: the block is rebuilt every
     * draw, and a callback that re-read the rows would re-run the model build on every poll of
     * every toggle. */
    button_func_pushed_state_set(
        but, [enabled = row->enabled](const ui::Button & /*button*/) { return enabled; });
    /* Faded the same way the row's own name and icon are -- whether that is because this layer's
     * own toggle is off, or because a group it sits in is: either way the toggle belongs to a row
     * that already reads as inactive, and should not be the one thing on it that still looks lit.
     * #BUT_INACTIVE only dims the button, unlike #BUT_DISABLED it stays clickable. */
    if (element_should_draw_faded(tvc, space_outliner, te, tselem)) {
      button_flag_enable(but, ui::BUT_INACTIVE);
    }
  });
}

/** Sets a block's emboss for the lifetime of the object and puts the previous one back. */
class StackScopedBlockEmboss {
  ui::Block *block_;
  ui::EmbossType previous_;

 public:
  StackScopedBlockEmboss(ui::Block *block, const ui::EmbossType emboss)
      : block_(block), previous_(block_emboss_get(block))
  {
    block_emboss_set(block_, emboss);
  }
  ~StackScopedBlockEmboss()
  {
    block_emboss_set(block_, previous_);
  }
};

/**
 * Dim a column button whose value is the inherited default rather than an override. The control
 * stays fully editable; only the text colour changes.
 */
static void stack_column_dim(ui::Button *button)
{
  const uchar dim[4] = {160, 160, 160, 255};
  button_drawflag_enable(button, ui::BUT_TEXT_USE_COL);
  button_color_set(button, dim);
}

/**
 * The popup a compact row's column label opens: the row's value slider and its blend mode.
 *
 * The argument is a copy -- the RNA pointers and property names -- not the row it came from, which
 * a rebuild may replace while the popup is open.
 */
struct StackColumnValueMenuArgs {
  PointerRNA value_ptr;
  PointerRNA mode_ptr;
  char value_prop[64];
  char mode_prop[64];
};

/** The value of \a prop as the "NN%" text a compact row's label shows. */
static void stack_column_value_label(PointerRNA &ptr, PropertyRNA *prop, char r_label[32])
{
  const float value = RNA_property_float_get(&ptr, prop);
  const float percent = (RNA_property_subtype(prop) == PROP_PERCENTAGE) ? value : value * 100.0f;
  BLI_snprintf(r_label, 32, "%d%%", int(percent + 0.5f));
}

/**
 * The compact row's one label: "NN%" and the first three letters of the mode name, whichever of
 * the two the row carries.
 */
static void stack_column_compact_label(bContext *C,
                                       const StackColumnValueMenuArgs &args,
                                       char r_label[64])
{
  char value_part[24] = "";
  char mode_part[24] = "";
  if (args.value_prop[0] != '\0') {
    PointerRNA ptr = args.value_ptr;
    PropertyRNA *prop = RNA_struct_find_property(&ptr, args.value_prop);
    if (prop != nullptr && RNA_property_type(prop) == PROP_FLOAT) {
      stack_column_value_label(ptr, prop, value_part);
    }
  }
  if (args.mode_prop[0] != '\0') {
    PointerRNA ptr = args.mode_ptr;
    PropertyRNA *prop = RNA_struct_find_property(&ptr, args.mode_prop);
    const char *name = nullptr;
    if (prop != nullptr && RNA_property_type(prop) == PROP_ENUM) {
      RNA_property_enum_name_gettexted(C, &ptr, prop, RNA_property_enum_get(&ptr, prop), &name);
    }
    if (name != nullptr) {
      const int byte_len = BLI_str_utf8_offset_from_index(name, strlen(name), 3);
      BLI_strncpy(mode_part, name, size_t(byte_len) + 1);
    }
  }
  if (value_part[0] != '\0' && mode_part[0] != '\0') {
    BLI_snprintf(r_label, 64, "%s %s", value_part, mode_part);
  }
  else if (value_part[0] != '\0') {
    BLI_strncpy(r_label, value_part, 64);
  }
  else {
    BLI_strncpy(r_label, mode_part, 64);
  }
}

/**
 * The popup block: a full-size integer-percent value slider and every mode as its own row.
 *
 * Unlike a menu, the popup stays open while the pointer is over it, so the value and the mode can
 * both be changed in one visit; it closes on mouse-leave or Escape.
 */
static ui::Block *stack_column_value_menu(bContext *C, ARegion *region, void *arg)
{
  StackColumnValueMenuArgs *args = static_cast<StackColumnValueMenuArgs *>(arg);
  ui::Block *block = block_begin(C, region, __func__, ui::EmbossType::Emboss);
  block_theme_style_set(block, ui::BLOCK_THEME_STYLE_POPUP);
  block_flag_enable(block, ui::BLOCK_KEEP_OPEN | ui::BLOCK_MOVEMOUSE_QUIT);
  const int width = int(OUTLINER_STACK_COLUMN_POPUP_WIDTH * UI_UNIT_X);
  const int height = int(UI_UNIT_Y);
  int y = 0;
  if (args->value_prop[0] != '\0') {
    PointerRNA ptr = args->value_ptr;
    PropertyRNA *prop = RNA_struct_find_property(&ptr, args->value_prop);
    if (prop != nullptr) {
      ui::Button *but = uiDefAutoButR(
          block, &ptr, prop, -1, "", ICON_NONE, 0, y, width, height, ui::ButtonType::NumSlider);
      if (but != nullptr) {
        /* Whole percent, as the row's label reads. */
        button_number_slider_precision_set(but, 0);
      }
    }
    y -= height;
  }
  if (args->mode_prop[0] != '\0') {
    PointerRNA ptr = args->mode_ptr;
    PropertyRNA *prop = RNA_struct_find_property(&ptr, args->mode_prop);
    if (prop != nullptr) {
      /* Every mode on its own menu-style row with a radio mark, as the Category Tab context menu's
       * Display Mode choices: the current one reads as the filled radio. */
      const int current = RNA_property_enum_get(&ptr, prop);
      const EnumPropertyItem *items = nullptr;
      bool free = false;
      RNA_property_enum_items_gettexted(C, &ptr, prop, &items, nullptr, &free);
      y -= int(0.4f * UI_UNIT_Y);
      const StackScopedBlockEmboss emboss_scope(block, ui::EmbossType::Pulldown);
      for (const EnumPropertyItem *item = items; item->identifier; item++) {
        if (item->identifier[0] == '\0') {
          continue;
        }
        ui::Button *but = uiDefIconTextButR_prop(block,
                                                 ui::ButtonType::Row,
                                                 (item->value == current) ? ICON_RADIOBUT_ON :
                                                                            ICON_RADIOBUT_OFF,
                                                 item->name,
                                                 0,
                                                 y,
                                                 width,
                                                 height,
                                                 &ptr,
                                                 prop,
                                                 -1,
                                                 0,
                                                 item->value,
                                                 std::nullopt);
        if (but != nullptr) {
          /* Left-align like a menu entry, set here rather than relying on the block flag that is
           * applied later, so the alignment is the same on the first draw and any refresh. */
          button_drawflag_enable(but, ui::BUT_TEXT_LEFT | ui::BUT_ICON_LEFT);
          /* A pick settles the mode, so the popup closes right away, like a menu. */
          ui::button_func_set(but,
                              [but](bContext & /*C*/) { ui::popup_menu_close_from_but(but); });
        }
        y -= height;
      }
      if (free) {
        MEM_delete(items);
      }
    }
  }
  block_bounds_set_popup(block, int(0.3f * UI_UNIT_Y), nullptr);
  return block;
}

/**
 * Open the column popup at the cursor for the clicked row.
 *
 * The row's value/mode RNA pointers are read here, when the operator runs, rather than carried on
 * the button: the rows are rebuilt every draw, so a button only names its ordinal.
 */
static wmOperatorStatus stack_column_popup_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent * /*event*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_rows_ensure(ctx, *space_outliner, *owner);
  const StackRow *row = outliner_stack_row_find(*space_outliner, RNA_int_get(op->ptr, "ordinal"));
  if (row == nullptr) {
    return OPERATOR_CANCELLED;
  }
  StackColumnValueMenuArgs *args = MEM_new<StackColumnValueMenuArgs>(__func__);
  args->value_prop[0] = '\0';
  args->mode_prop[0] = '\0';
  if (row->value_ptr && row->value_prop != nullptr) {
    args->value_ptr = *row->value_ptr;
    STRNCPY(args->value_prop, row->value_prop);
  }
  if (row->mode_ptr && row->mode_prop != nullptr) {
    args->mode_ptr = *row->mode_ptr;
    STRNCPY(args->mode_prop, row->mode_prop);
  }
  ui::popup_block_invoke_ex(
      C, stack_column_value_menu, args, ui::but_func_argN_free<StackColumnValueMenuArgs>, false);
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_column_popup(wmOperatorType *ot)
{
  ot->name = "Edit Stack Column";
  ot->idname = "OUTLINER_OT_stack_column_popup";
  ot->description = "Edit the Stack Layer row's value and mode in a popup";
  ot->invoke = stack_column_popup_invoke;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_INTERNAL;

  RNA_def_int(ot->srna, "ordinal", 0, 0, SHRT_MAX, "Ordinal", "", 0, SHRT_MAX);
}

struct StackColumnsDraw {
  bool show_value;
  bool show_mode;
  bool show_action;
  /**
   * With Large rows on, the mode and the value stack in one column instead of sitting side by
   * side: the row is two units tall, room enough for one full-height button above the other. Only
   * one of the two is ever visible there, so the column -- and the buttons in it -- are only as
   * wide as the wider of the two, not both added together. A compact row is one unit tall and
   * never stacks; that is decided per row.
   */
  bool stacked_layout;
  bool editable;
  /** Column widths come in UI units and may be fractional, so they stay floats until the button
   * rects round them to pixels. */
  float column_x;
  float value_width;
  float mode_width;
  float stacked_width;
  float action_width;
};

/** Where the controls of one row go. */
struct StackColumnsRowRects {
  bool stacked;
  int value_y;
  int mode_y;
  /** The height of the value and mode buttons. */
  int button_height;
  float value_x;
  float mode_x;
  float value_width;
  float mode_width;
  float action_x;
  /** The width of the one label a compact row shows in place of the two columns. */
  float compact_width;
};

static StackColumnsRowRects stack_columns_row_rects(const StackColumnsDraw &draw,
                                                    const SpaceOutliner &space_outliner,
                                                    const TreeElement &te,
                                                    const StackRow &row)
{
  StackColumnsRowRects rects;
  rects.stacked = draw.stacked_layout && !row.compact;

  /* Full #UI_UNIT_Y tall and centered on the row's single content line normally; stacked, each
   * shrinks a little instead of filling its half of the tall row edge to edge, so the pair reads
   * as one compact control rather than two stretched widgets touching in the middle. `te.ys` is
   * the row's own bottom, written by the tree pass that ran before this one. */
  const int unit_height = rects.stacked ?
                              int(UI_UNIT_Y * OUTLINER_STACK_COLUMN_STACKED_BUTTON_HEIGHT) :
                              int(UI_UNIT_Y);
  const int pad_y = int(OUTLINER_STACK_COLUMN_BUTTON_PAD_Y * UI_SCALE_FAC);
  if (rects.stacked) {
    const int row_height = outliner_tree_element_height(space_outliner, te);
    const int gap = int(OUTLINER_STACK_COLUMN_STACKED_GAP * UI_SCALE_FAC);
    const int margin = (row_height - 2 * unit_height - gap) / 2;
    rects.mode_y = te.ys + row_height - margin - unit_height;
    rects.value_y = te.ys + margin;
  }
  else {
    rects.mode_y = stack_row_content_y(space_outliner, te) + pad_y;
    rects.value_y = rects.mode_y;
  }
  /* A compact row is one unit tall, less a pixel so its frame does not touch its neighbors. */
  if (row.compact) {
    rects.button_height = int(UI_UNIT_Y) - 1 - 2 * pad_y;
  }
  else {
    rects.button_height = rects.stacked ? unit_height : unit_height - 2 * pad_y;
  }

  /* Side by side, value keeps its usual place on the left of mode; stacked, both sit in the same
   * single column and each fills the combined width the column was reserved with. */
  rects.value_x = draw.column_x;
  rects.mode_x = rects.stacked ? draw.column_x : draw.column_x + draw.value_width;
  rects.value_width = rects.stacked ? draw.stacked_width : draw.value_width;
  rects.mode_width = rects.stacked ? draw.stacked_width : draw.mode_width;
  /* A compact row shows one label in that same column: the stacked column's width when the rows
   * stack, both columns' width otherwise. */
  rects.compact_width = draw.stacked_layout ? draw.stacked_width :
                                              (draw.value_width + draw.mode_width);
  /* The action goes after the value and mode columns, compact rows included. */
  float action_offset = draw.value_width + draw.mode_width;
  if (row.compact) {
    action_offset = rects.compact_width;
  }
  else if (rects.stacked) {
    action_offset = draw.stacked_width;
  }
  rects.action_x = draw.column_x + action_offset;
  return rects;
}

/** What every column button gets once it exists: the smaller text, the inherited-value dimming,
 * and the disabled state of a stack that cannot be edited. */
static void stack_column_button_finish(ui::Button *button,
                                       const bool inherited,
                                       const bool editable)
{
  if (button == nullptr) {
    return;
  }
  ui::button_text_scale_set(button, OUTLINER_STACK_COLUMN_TEXT_SCALE);
  if (inherited) {
    stack_column_dim(button);
  }
  if (!editable) {
    button_disable(button, N_("This stack cannot be edited"));
  }
}

/** The row's own action: one unit square, centered on the row's full height whether or not the
 * row stacks. */
static void stack_column_action_button(ui::Block *block,
                                       const SpaceOutliner &space_outliner,
                                       const TreeElement &te,
                                       const StackRow &row,
                                       const int ordinal,
                                       const StackColumnsDraw &draw,
                                       const StackColumnsRowRects &rects)
{
  const int height = int(UI_UNIT_Y);
  const int y = te.ys + (outliner_tree_element_height(space_outliner, te) - height) / 2;
  /* A plain operator button: a toggle type without an RNA property never runs the operator.
   * Embossed so the full two-unit width reads as one button, not a bare icon. */
  const StackScopedBlockEmboss emboss_scope(block, ui::EmbossType::Emboss);
  /* Same button kind and call context as the row's visibility toggle, which is known to fire. */
  ui::Button *button = uiDefIconButO(block,
                                     ui::ButtonType::ButToggle,
                                     "OUTLINER_OT_stack_row_action",
                                     wm::OpCallContext::ExecDefault,
                                     row.action_icon,
                                     int(rects.action_x),
                                     y,
                                     int(draw.action_width),
                                     height,
                                     row.action_tooltip ? row.action_tooltip : "");
  if (button == nullptr) {
    return;
  }
  RNA_int_set(ui::button_operator_ptr_ensure(button), "ordinal", ordinal);
  /* Captured by value: the block is rebuilt every draw. */
  button_func_pushed_state_set(
      button, [active = row.action_active](const ui::Button & /*button*/) { return active; });
  if (!draw.editable) {
    button_disable(button, N_("This stack cannot be edited"));
  }
}

/** One label for the row's columns; its button opens #OUTLINER_OT_stack_column_popup, which reads
 * the row's value/mode RNA when it runs -- the rows are rebuilt every draw. */
static void stack_column_compact_button(bContext *C,
                                        ui::Block *block,
                                        const StackRow &row,
                                        const int ordinal,
                                        const StackColumnsDraw &draw,
                                        const StackColumnsRowRects &rects)
{
  StackColumnValueMenuArgs label_args;
  label_args.value_prop[0] = '\0';
  label_args.mode_prop[0] = '\0';
  if (draw.show_value && row.value_ptr && row.value_prop != nullptr) {
    label_args.value_ptr = *row.value_ptr;
    STRNCPY(label_args.value_prop, row.value_prop);
  }
  if (draw.show_mode && row.mode_ptr && row.mode_prop != nullptr) {
    label_args.mode_ptr = *row.mode_ptr;
    STRNCPY(label_args.mode_prop, row.mode_prop);
  }
  char label[64];
  stack_column_compact_label(C, label_args, label);

  ui::Button *button = nullptr;
  {
    const StackScopedBlockEmboss emboss_scope(block, ui::EmbossType::None);
    button = uiDefButO(block,
                       ui::ButtonType::But,
                       "OUTLINER_OT_stack_column_popup",
                       wm::OpCallContext::InvokeDefault,
                       label,
                       int(draw.column_x),
                       rects.value_y,
                       int(rects.compact_width),
                       rects.button_height,
                       std::nullopt);
  }
  if (button == nullptr) {
    return;
  }
  RNA_int_set(ui::button_operator_ptr_ensure(button), "ordinal", ordinal);
  stack_column_button_finish(button, row.value_inherited || row.mode_inherited, draw.editable);
}

/** The value column's button: the row's RNA property, as a whole-percent slider where it is one.
 */
static ui::Button *stack_column_value_button(ui::Block *block,
                                             const StackRow &row,
                                             const StackColumnsRowRects &rects)
{
  if (!row.value_ptr || row.value_prop == nullptr) {
    return nullptr;
  }
  PointerRNA rna_ptr = *row.value_ptr;
  PropertyRNA *prop = RNA_struct_find_property(&rna_ptr, row.value_prop);
  if (prop == nullptr) {
    return nullptr;
  }
  ui::Button *button = uiDefAutoButR(block,
                                     &rna_ptr,
                                     prop,
                                     -1,
                                     "",
                                     ICON_NONE,
                                     int(rects.value_x),
                                     rects.value_y,
                                     int(rects.value_width),
                                     rects.button_height);
  /* Whole percent, as the row's label and the popup slider read. */
  if (button != nullptr && ELEM(RNA_property_subtype(prop), PROP_FACTOR, PROP_PERCENTAGE)) {
    button_number_slider_precision_set(button, 0);
  }
  return button;
}

/** The mode column's button: the row's RNA property under its own name. */
static ui::Button *stack_column_mode_button(ui::Block *block,
                                            const StackRow &row,
                                            const StackColumnsRowRects &rects)
{
  if (!row.mode_ptr || row.mode_prop == nullptr) {
    return nullptr;
  }
  PointerRNA rna_ptr = *row.mode_ptr;
  PropertyRNA *prop = RNA_struct_find_property(&rna_ptr, row.mode_prop);
  if (prop == nullptr) {
    return nullptr;
  }
  return uiDefAutoButR(block,
                       &rna_ptr,
                       prop,
                       -1,
                       std::nullopt,
                       ICON_NONE,
                       int(rects.mode_x),
                       rects.mode_y,
                       int(rects.mode_width),
                       rects.button_height);
}

float outliner_stack_columns_width(const SpaceOutliner &space_outliner)
{
  if (space_outliner.stack_layers_view != SO_SL_VIEW_STACK) {
    return 0.0f;
  }
  /* How wide the two columns are is the source's call: an opacity slider and a shape key value
   * want different room than a blend-mode menu. With Large rows on the two stack in one column
   * instead of sitting side by side, but only one of the two is ever visible at a time there, so
   * the column only has to be as wide as the wider of the two, not both together. */
  const StackColumnLayout layout = stack_source_for_space(space_outliner)->column_layout();
  const bool show_value = layout.value_width > 0 &&
                          (space_outliner.stack_layers_flag & SO_SL_HIDE_VALUE) == 0;
  const bool show_mode = layout.mode_width > 0 &&
                         (space_outliner.stack_layers_flag & SO_SL_HIDE_BLEND) == 0;
  const bool stacked = show_value && show_mode &&
                       (space_outliner.stack_layers_flag & SO_SL_SMALL_ROWS) == 0;
  float num_columns = 0.0f;
  /* A compact row shows its value and its mode in one label inside the same column, so it takes
   * no more room than the columns already reserve. */
  if (stacked) {
    num_columns += max_ff(layout.value_width, layout.mode_width);
  }
  else {
    if (show_value) {
      num_columns += layout.value_width;
    }
    if (show_mode) {
      num_columns += layout.mode_width;
    }
  }
  /* The action column is not one of the two the flags hide. */
  num_columns += max_ff(layout.action_width, 0.0f);
  /* The per-row toggles live to the right of those columns, the way the restriction icons do in
   * the View Layer, rather than trailing the name -- unless the visibility toggle was moved to the
   * tree's own first column, in which case nothing is left to reserve room for. */
  if ((space_outliner.stack_layers_flag & SO_SL_VISIBILITY_RIGHT) != 0) {
    num_columns += layout.icon_columns;
  }
  /* No half-unit of air here, unlike the restriction columns: the stack's value and mode buttons
   * go flush against the scrollbar strip, which is the only thing reserved past them. This width
   * is still the one the name's clipping, the scrollable width and the hit test measure from --
   * they have to agree with where the buttons ended up. */
  return num_columns * UI_UNIT_X + V2D_SCROLL_WIDTH;
}

void outliner_draw_stack_columns(bContext *C,
                                 ui::Block *block,
                                 ARegion *region,
                                 SpaceOutliner *space_outliner,
                                 const TreeViewContext &tvc)
{
  const StackSource &source = *stack_source_for_space(*space_outliner);
  const StackColumnLayout layout = source.column_layout();
  StackColumnsDraw draw{};
  draw.show_value = layout.value_width > 0 &&
                    (space_outliner->stack_layers_flag & SO_SL_HIDE_VALUE) == 0;
  draw.show_mode = layout.mode_width > 0 &&
                   (space_outliner->stack_layers_flag & SO_SL_HIDE_BLEND) == 0;
  /* Not hidden by the value/mode flags: the action is not either of those columns. */
  draw.show_action = layout.action_width > 0;
  if (!draw.show_value && !draw.show_mode && !draw.show_action) {
    return;
  }

  const StackReadContext ctx = {tvc.bmain, tvc.scene, tvc.view_layer};
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return;
  }
  draw.editable = source.is_editable(*owner);
  draw.stacked_layout = draw.show_value && draw.show_mode &&
                        (space_outliner->stack_layers_flag & SO_SL_SMALL_ROWS) == 0;

  /* The margin from the right edge is part of that width; see #outliner_right_columns_width. */
  draw.column_x = region->v2d.cur.xmax - outliner_right_columns_width(space_outliner);
  draw.value_width = draw.show_value ? layout.value_width * UI_UNIT_X : 0;
  draw.mode_width = draw.show_mode ? layout.mode_width * UI_UNIT_X : 0;
  draw.stacked_width = max_ff(draw.value_width, draw.mode_width);
  draw.action_width = draw.show_action ? layout.action_width * UI_UNIT_X : 0;

  tree_iterator::all_open(*space_outliner, [&](TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type != TSE_STACK_LAYER ||
        !outliner_is_element_in_view(*space_outliner, te, &region->v2d))
    {
      return;
    }
    const StackRow *row = outliner_stack_row_find(*space_outliner, tselem->nr);
    if (row == nullptr || !row->supported) {
      return;
    }
    const StackColumnsRowRects rects = stack_columns_row_rects(draw, *space_outliner, *te, *row);

    if (draw.show_action && row->action_icon != ICON_NONE) {
      stack_column_action_button(block, *space_outliner, *te, *row, tselem->nr, draw, rects);
    }
    if (row->compact) {
      stack_column_compact_button(C, block, *row, tselem->nr, draw, rects);
      return;
    }
    if (draw.show_value) {
      stack_column_button_finish(
          stack_column_value_button(block, *row, rects), row->value_inherited, draw.editable);
    }
    if (draw.show_mode) {
      stack_column_button_finish(
          stack_column_mode_button(block, *row, rects), row->mode_inherited, draw.editable);
    }
  });
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

/** The state one walk over the tree's rows carries while it draws the bands. */
struct StackBandsDraw {
  const ARegion *region;
  const SpaceOutliner *space_outliner;
  bool pair_sub_rows;
  uint pos;
  const float *col_alternate;
  const float *col_divider;
  /** Odd values are shaded. Counted in complete tree elements, not in units of height. */
  int stripe_index;
  /** The top of the next row, in the same space as #TreeElement::ys. */
  int start_y;
};

/** A full-width alternate band from \a y up by \a height. */
static void stack_band_fill(const StackBandsDraw &draw, const int y, const int height)
{
  immUniformColor4fv(draw.col_alternate);
  immRectf(draw.pos,
           float(draw.region->v2d.cur.xmin),
           float(y),
           float(draw.region->v2d.cur.xmax),
           float(y + height));
}

/** A one-pixel rule from \a x to the right edge, whose bottom is at \a y. */
static void stack_band_rule(const StackBandsDraw &draw, const float x, const float y)
{
  immUniformColor4fv(draw.col_divider);
  immRectf(draw.pos, x, y, float(draw.region->v2d.cur.xmax), y + U.pixelsize);
}

/**
 * Whether \a te is a row the source attaches under another row's own section (a correction on
 * its content, a nested view of its alternate content).
 */
static bool stack_row_is_attached(const SpaceOutliner &space_outliner, const TreeElement *te)
{
  if (te == nullptr || TREESTORE(te)->type != TSE_STACK_LAYER) {
    return false;
  }
  const StackRow *row = outliner_stack_row_find(space_outliner, TREESTORE(te)->nr);
  return row != nullptr && !row->parent_section_id.empty();
}

/**
 * Draw the sub-rows of \a te as pairs: [1+2], [3+4], ...; for an odd count the first row is kept
 * on its own: [1], [2+3], [4+5], ... . Returns false when \a te holds no sub-row, so the caller
 * walks it the regular way.
 */
static bool stack_bands_paired_sub_rows(StackBandsDraw &draw,
                                        const TreeElement &te,
                                        const int startx)
{
  int sub_row_count = 0;
  for (const TreeElement &child : te.subtree) {
    sub_row_count += TREESTORE(&child)->type == TSE_STACK_ITEM;
  }
  if (sub_row_count == 0) {
    return false;
  }

  const int sub_row_x = startx + UI_UNIT_X;
  const int first_stripe_index = draw.stripe_index;
  int sub_row_index = 0;
  for (const TreeElement &child : te.subtree) {
    if (TREESTORE(&child)->type != TSE_STACK_ITEM) {
      continue;
    }
    const int group = (sub_row_count & 1) ?
                          (sub_row_index == 0 ? 0 : 1 + (sub_row_index - 1) / 2) :
                          sub_row_index / 2;
    const int height = outliner_tree_element_height(*draw.space_outliner, child);
    const int y = draw.start_y + UI_UNIT_Y - height;
    if (((first_stripe_index + group) & 1) != 0) {
      stack_band_fill(draw, y, height);
    }
    if (sub_row_index != 0) {
      stack_band_rule(draw, float(sub_row_x), float(y + height) - U.pixelsize);
    }
    sub_row_index++;
    draw.start_y -= height;
  }
  draw.stripe_index = first_stripe_index + (sub_row_count + 1) / 2;
  return true;
}

/**
 * Draw alternating Stack Layers backgrounds and the rule between a row's sub-rows, from the
 * actual row geometry.
 *
 * The regular Outliner background uses a fixed one-unit pitch. That cuts a two-unit row in half,
 * so Stack Layers alternates after complete tree elements instead. Sub-rows are one unit high and
 * are therefore handled naturally. In pair mode, the sub-rows a #StackRow pairs (see
 * #StackRow::pairs_sub_rows) are grouped, see #stack_bands_paired_sub_rows.
 */
static void stack_bands_recursive(StackBandsDraw &draw,
                                  const ListBaseT<TreeElement> &lb,
                                  const int startx)
{
  const SpaceOutliner &space_outliner = *draw.space_outliner;
  for (const TreeElement &te : lb) {
    const TreeStoreElem *tselem = TREESTORE(&te);
    const int row_height = outliner_tree_element_height(space_outliner, te);
    const int row_y = draw.start_y + UI_UNIT_Y - row_height;
    const float row_top = float(row_y + row_height);

    /* The band is drawn first so the rule below always shows on top of it, whichever of the two
     * rows on either side of the rule is the shaded one. */
    if ((draw.stripe_index & 1) != 0) {
      stack_band_fill(draw, row_y, row_height);
    }

    /* A rule between the sub-rows a row's content is made of. They are one-line rows of the same
     * shape, and without a line between them they read as one block of text. The first one needs
     * none: the row they hang under is above it. Starting at the row's own indent rather than the
     * region edge keeps it off the hierarchy line and expand-arrow gutter to its left. */
    if (tselem->type == TSE_STACK_ITEM && te.prev != nullptr) {
      stack_band_rule(draw, float(startx), row_top - U.pixelsize);
    }

    /* Attached rows need the same rule: one above the first row of the run and one below the
     * last, so the group reads as a block apart from the host row's own sub-rows. They are drawn
     * one indent to the left, see #stack_child_start_x. */
    const bool attached = stack_row_is_attached(space_outliner, &te);
    const int row_startx = attached ?
                               stack_child_start_x(space_outliner, te, startx - int(UI_UNIT_X)) :
                               startx;
    if (attached) {
      /* On every attached row, so the rule falls between the rows as well as above the first. */
      stack_band_rule(draw, float(row_startx), row_top - U.pixelsize);
    }
    const bool attached_run_ends = attached && !stack_row_is_attached(space_outliner, te.next);
    /* Drawn once the row's own subtree is listed, so the rule sits under a folder correction's
     * children and not between it and them. The next row's top edge is where it goes; it takes
     * the last pixel row above it so that row's own band cannot paint over it. */
    const auto draw_run_end_rule = [&]() {
      if (attached_run_ends) {
        stack_band_rule(draw, float(row_startx), float(draw.start_y + UI_UNIT_Y));
      }
    };

    draw.stripe_index++;
    draw.start_y -= row_height;

    if (!TSELEM_OPEN(tselem, &space_outliner)) {
      draw_run_end_rule();
      continue;
    }

    const StackRow *row = (tselem->type == TSE_STACK_LAYER) ?
                              outliner_stack_row_find(space_outliner, tselem->nr) :
                              nullptr;
    if (row != nullptr && draw.pair_sub_rows && row->pairs_sub_rows &&
        stack_bands_paired_sub_rows(draw, te, startx))
    {
      draw_run_end_rule();
      continue;
    }

    stack_bands_recursive(draw, te.subtree, row_startx + UI_UNIT_X);
    draw_run_end_rule();
  }
}

void outliner_draw_stack_row_bands(const ARegion *region,
                                   const SpaceOutliner *space_outliner,
                                   const int startx)
{
  if (space_outliner->outlinevis != SO_STACK_LAYERS ||
      space_outliner->stack_layers_view != SO_SL_VIEW_STACK)
  {
    return;
  }

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);

  /* Themes saved before #TH_STACK_ALTERNATE existed leave it unset (alpha zero); fall back to
   * the regular Outliner alternate-row color instead of drawing nothing until the user
   * re-saves the theme. */
  float col_stack_alternate[4];
  ui::theme::get_color_4fv(TH_STACK_ALTERNATE, col_stack_alternate);
  const int alternate_theme_id = (col_stack_alternate[3] > 0.0f) ? TH_STACK_ALTERNATE :
                                                                   TH_ROW_ALTERNATE;
  float col_alternate_raw[4];
  ui::theme::get_color_4fv(alternate_theme_id, col_alternate_raw);
  float col_alternate[4];
  ui::theme::get_color_blend_3f(TH_BACK, alternate_theme_id, col_alternate_raw[3], col_alternate);
  col_alternate[3] = 1.0f;

  float col_divider[4];
  ui::theme::get_color_blend_4f(TH_TEXT, TH_BACK, 0.8f, col_divider);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Start with the alternate color so the first stack row remains distinct from a same-colored
   * header and background in themes that use identical values for both. */
  StackBandsDraw draw{};
  draw.region = region;
  draw.space_outliner = space_outliner;
  draw.pair_sub_rows = (space_outliner->stack_layers_flag & SO_SL_PAIR_SUB_ROWS) != 0;
  draw.pos = pos;
  draw.col_alternate = col_alternate;
  draw.col_divider = col_divider;
  draw.stripe_index = 1;
  draw.start_y = int(region->v2d.tot.ymax) - UI_UNIT_Y - OL_Y_OFFSET;
  stack_bands_recursive(draw, space_outliner->runtime->tree, startx);

  /* The walk above only covers the tree's own rows. Below the last one, tile the same alternate
   * band on the Outliner's usual one-unit pitch so an empty stretch of the list reads the same
   * way it always has, rather than as a plain, un-striped background. */
  while (draw.start_y + UI_UNIT_Y > region->v2d.cur.ymin) {
    if ((draw.stripe_index & 1) != 0) {
      stack_band_fill(draw, draw.start_y, UI_UNIT_Y);
    }
    draw.stripe_index++;
    draw.start_y -= UI_UNIT_Y;
  }

  immUnbindProgram();
}

}  // namespace ed::outliner
}  // namespace blender
