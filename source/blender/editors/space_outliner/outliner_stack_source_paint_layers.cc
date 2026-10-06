/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The paint layers of a *layered* material, read from and written to the DNA description.
 *
 * Where #PaintMaterialStackSource is a reader of the node graph, this source owns nothing but the
 * description: `Material::paint_layers` is the truth, the node tree is generated from it, and every
 * verb here goes through the `BKE_paint_layers_*` API by marker. It is deliberately free of the
 * old graph-as-truth vocabulary -- no bindings, no corrections of a graph, no node identities --
 * so the description can be edited with or without an Outliner in the context (P-1).
 *
 * #PaintMaterialStackSource delegates to this source whenever the owner is a layered material
 * (#paint_layers_is_layered), so the two implementations stay behind the one source type the space
 * registers.
 */

#include <algorithm>
#include <climits>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "AS_asset_representation.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_uuid_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "BLI_assert.h"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_fileops.hh"
#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "IMB_imbuf_types.hh"

#include "ED_asset_image_utils.hh"
#include "ED_asset_import.hh"
#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_paint_material_layer.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "outliner_intern.hh"
#include "outliner_stack_source.hh"
#include "outliner_stack_source_paint_layers_intern.hh"

namespace blender::ed::outliner {

namespace {

/** The owner of a layered paint stack is always a material. */
Material &layers_owner(ID &owner)
{
  BLI_assert(GS(owner.name) == ID_MA);
  return id_cast<Material &>(owner);
}

const Material &layers_owner(const ID &owner)
{
  BLI_assert(GS(owner.name) == ID_MA);
  return id_cast<const Material &>(owner);
}

/**
 * Bring \a image up in an Image Editor, turning the Outliner's own area into one when no Image
 * Editor is open. The same landing point a channel map uses.
 */
bool paint_layers_image_open(bContext &C, ID *image)
{
  if (image == nullptr) {
    return true;
  }
  ScrArea *area = outliner_image_area_find(C);
  if (area == nullptr) {
    area = CTX_wm_area(&C);
    if (area == nullptr) {
      return false;
    }
    BKE_report(CTX_wm_reports(&C),
               RPT_INFO,
               "No Image Editor was open, so this area was turned into one");
    ED_area_newspace(&C, area, SPACE_IMAGE, false);
  }
  SpaceImage *space_image = static_cast<SpaceImage *>(area->spacedata.first);
  ED_space_image_set(CTX_data_main(&C), space_image, id_cast<Image *>(image), false);
  WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_IMAGE, space_image);
  return true;
}

/** The channel the Stack Layers header selected, from the scene's paint settings; clamped. */
int paint_stack_selected_channel(const StackReadContext &ctx)
{
  if (ctx.scene == nullptr || ctx.scene->toolsettings == nullptr) {
    return PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  }
  const int channel = ctx.scene->toolsettings->paint_mode.stack_layer_channel;
  return (channel >= 0 && channel < PAINT_MATERIAL_CHANNEL_NUM) ? channel :
                                                                  PAINT_MATERIAL_CHANNEL_BASE_COLOR;
}

/**
 * The ordinal #paint_stack_rows_from_description would give \a target, or -1. The order is the one
 * #BKE_paint_layers_foreach defines, which is what the row builder's #append_corrections appends
 * in; the base mask item consumes an ordinal without a row of its own here too.
 */
int layers_ordinal_of(const Material &ma, const MaterialPaintLayer *target)
{
  if (target == nullptr) {
    return -1;
  }
  int index = 0;
  int found = -1;
  BKE_paint_layers_foreach(ma,
                           [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/)
                               -> bool {
                             if (&layer == target) {
                               found = index;
                               return false;
                             }
                             index++;
                             return true;
                           });
  return found;
}

/** Set a layered material's target mode, resolving the active Paint the same way the operator
 * does. */
bool layers_target_mode_set(bContext &C, const ePaintLayerTargetMode mode)
{
  Main *bmain = CTX_data_main(&C);
  Scene *scene = CTX_data_scene(&C);
  Paint *paint = BKE_paint_get_active_from_context(&C);
  if (bmain == nullptr || scene == nullptr || scene->toolsettings == nullptr || paint == nullptr) {
    return false;
  }
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  if (paint_mode.layer_target_mode == mode) {
    return false;
  }
  BKE_paint_material_layer_target_mode_set(*bmain, *scene, *paint, paint_mode, mode);
  return true;
}

int layers_channel_role_channel(const StackSubRow &sub_row)
{
  return sub_row.role;
}

}  // namespace

/* Row helpers for a layered material: the description's own rows, previews and icons.
 * Moved here from the graph source so the description side no longer depends on it. */
namespace {
int paint_channel_icon(const int channel)
{
  switch (eMaterialPaintChannel(channel)) {
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
      return ICON_IMAGE_RGB;
    case PAINT_MATERIAL_CHANNEL_METALLIC:
      return ICON_NODE_MATERIAL;
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
      return ICON_MOD_NOISE;
    case PAINT_MATERIAL_CHANNEL_SPECULAR:
      return ICON_INDIRECT_ONLY_ON;
    case PAINT_MATERIAL_CHANNEL_NORMAL:
      return ICON_NORMALS_FACE;
    case PAINT_MATERIAL_CHANNEL_HEIGHT:
      return ICON_MOD_DISPLACE;
    case PAINT_MATERIAL_CHANNEL_ALPHA:
      return ICON_IMAGE_ALPHA;
    case PAINT_MATERIAL_CHANNEL_AO:
      return ICON_SHADING_RENDERED;
    case PAINT_MATERIAL_CHANNEL_EMISSION:
      return ICON_LIGHT;
    case PAINT_MATERIAL_CHANNEL_CUSTOM:
      break;
  }
  return ICON_IMAGE_DATA;
}

bool paint_image_is_blank(const Image &image)
{
  if (image.source != IMA_SRC_GENERATED) {
    return false;
  }
  const ImageTile *base_tile = BKE_image_get_tile(const_cast<Image *>(&image), 0);
  return base_tile != nullptr && base_tile->gen_type == IMA_GENTYPE_BLANK &&
         base_tile->gen_color[3] == 0.0f && !BKE_image_is_dirty(const_cast<Image *>(&image));
}

static int paint_mask_state_icon(const bool enabled)
{
  return enabled ? ICON_MOD_MASK : ICON_CLIPUV_HLT;
}

/**
 * The mask slot of a row that has one, and the section it opens.
 *
 * \param keeps_row_icon: for a group, its folder icon stays ahead of the mask thumbnail. The
 * caller knows what the row is; the slot only records it for the draw.
 */
StackRowPreview paint_mask_slot_build(const bool keeps_row_icon, const bool enabled)
{
  StackRowPreview slot;
  slot.section_id = "MASK";
  /* A switched-off mask reads as one at a glance: the icon says the row's coverage no longer
   * comes from it. */
  slot.icon = paint_mask_state_icon(enabled);
  slot.keeps_row_icon = keeps_row_icon;
  /* Whether the mask began black or white is unreadable once it has been painted over, so the
   * label stays neutral rather than guessing from the initial fill color. */
  slot.label = IFACE_("Mask");
  return slot;
}

/**
 * The MASK section of \a layer.
 *
 * The section exists as soon as the layer has any mask item, even a constant one with no map: its
 * presence is what lets a click switch the brush to the mask target. Its content is the items
 * themselves -- the source lists them as rows attached to this section by #append_corrections --
 * so the section carries no sub-rows of its own. Listing each item's map here as well put a second
 * "Mask" row beside every item.
 */
StackContentSection paint_mask_section_build(const Material & /*ma*/,
                                             const MaterialPaintLayer & /*layer*/)
{
  StackContentSection section;
  section.identifier = "MASK";
  section.name = IFACE_("Mask Content");
  return section;
}

void paint_stack_rows_from_description_impl(const Material &material,
                                            const int channel,
                                            Vector<StackRow> &r_rows)
{
  int ordinal = 0;
  bool overflow = false;

  /* The value/mode columns of \a row point at the fixed per (row, channel) settings entry, which
   * exists for every channel: the grid is a real DNA array, so the RNA path is stable and a
   * keyframe can address it whether or not the pair has a channel record yet. A mask correction
   * lays coverage with the over formula, so its blend plays no part: only its row opacity shows,
   * and no mode column is offered. */
  auto set_pair_columns = [&](StackRow &row, const MaterialPaintLayer &layer) {
    const bool is_mask_correction = BKE_paint_layers_role(layer) == PaintLayerRole::MaskItem;
    if (is_mask_correction) {
      PointerRNA layer_ptr = RNA_pointer_create_discrete(
          &const_cast<Material &>(material).id,
          RNA_struct_find("MaterialPaintLayer"),
          const_cast<MaterialPaintLayer *>(&layer));
      row.value_ptr = layer_ptr;
      row.value_prop = "opacity";
      row.value_inherited = layer.opacity == 1.0f;
      /* The item's own blend, one of the six modes the mask coverage stack reads. */
      row.mode_ptr = layer_ptr;
      row.mode_prop = "blend_type";
      return;
    }
    /* The Normal channel offers Mix (the forced combine) or Replace through the same column; its
     * "Mix" is the stored inherit value, so it is a real choice and is never dimmed as inherited. */
    const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
    const MaterialPaintLayerChannelSettings &settings = layer.channel_settings[channel];

    PointerRNA settings_ptr = RNA_pointer_create_discrete(
        &const_cast<Material &>(material).id,
        RNA_struct_find("MaterialPaintLayerChannelSettings"),
        const_cast<MaterialPaintLayerChannelSettings *>(&settings));
    row.value_ptr = settings_ptr;
    row.value_prop = "opacity";
    /* The flags only dim the columns; they no longer change how the control is built. */
    row.value_inherited = settings.opacity == 1.0f;
    row.mode_ptr = settings_ptr;
    row.mode_prop = "blend_type";
    row.mode_inherited = !is_normal && settings.blend < 0;
  };

  /* The description's own problems -- a folder carrying channels, a Fill correction on Normal and
   * so on -- are shown on the row they are about, as a warning icon with the reason in its tooltip.
   * The list is built once: it is cheap, but a row would otherwise ask the whole description per
   * row. */
  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(material, issues);

  auto append_issue_slots = [&](StackRow &row,
                                const bUUID &layer_marker,
                                const bUUID &correction_marker) {
    for (const PaintLayersIssue &issue : issues) {
      if (!BLI_uuid_equal(issue.layer, layer_marker)) {
        continue;
      }
      if (BLI_uuid_is_nil(correction_marker)) {
        if (!BLI_uuid_is_nil(issue.correction)) {
          continue;
        }
      }
      else if (!BLI_uuid_equal(issue.correction, correction_marker)) {
        continue;
      }
      StackRowPreview warning;
      warning.icon = ICON_ERROR;
      warning.label = issue.text != nullptr ? issue.text : "";
      row.preview_slots.append(std::move(warning));
    }
  };

  /* A Material row shows no Live/Baked indicator: how it is currently drawn is not the user's
   * concern. Only a refusal (the source cannot be shown at all) is worth a row icon; the answer is
   * the same BKE one the Source Material panel reads. */
  auto append_material_refusal_slot = [&](StackRow &row, const MaterialPaintLayer &source_row) {
    PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
    if (BKE_paint_layers_material_live_status(material, source_row, &refusal) !=
        PaintLayerMaterialLiveStatus::Refused)
    {
      return;
    }
    StackRowPreview refused_slot;
    refused_slot.icon = ICON_ERROR;
    refused_slot.label = "Refused: " +
                         std::string(BKE_paint_layers_source_group_refusal_name(refusal));
    row.preview_slots.append(std::move(refused_slot));
  };

  /* Forward-declared so #append_corrections can recurse into a Stack correction/mask item's own
   * children (Phase 6, goal 4): the two lambdas call each other, and a `std::function` variable can
   * be referenced before it is assigned as long as nothing actually calls it that early. */
  std::function<void(const ListBaseT<MaterialPaintLayer> &, int, int)> append_list;

  /* Corrections of \a layer, split by their role so an effect lists under CHANNELS and a mask item
   * under MASK, in storage order. A Stack-sourced correction/mask item is a folder exactly like a
   * Stack Layer row (#BKE_paint_layers_is_folder keys off `source`, not `role`): its own children
   * are appended the same way a folder Layer's are, with the correction's row as their parent. */
  auto append_corrections = [&](const MaterialPaintLayer &layer,
                                const int parent_ordinal,
                                const int parent_depth,
                                const int8_t role) {
    const Vector<const MaterialPaintLayer *> corrections =
        (role == MA_PAINT_LAYER_ROLE_MASK_ITEM) ? BKE_paint_layers_mask_items(layer) :
                                                  BKE_paint_layers_effects(layer);
    for (const MaterialPaintLayer *correction_ptr : corrections) {
      const MaterialPaintLayer &correction = *correction_ptr;
      if (ordinal > STACK_ROW_ORDINAL_MAX) {
        overflow = true;
        return;
      }
      if (role == MA_PAINT_LAYER_ROLE_MASK_ITEM &&
          correction_ptr == BKE_paint_layers_mask_base(layer))
      {
        /* The base mask is drawn by the layer row's own mask slot, not as a row of its own. Its
         * ordinal is still consumed, so the rows after it keep the numbering
         * #paint_description_row_for_ordinal hands back. */
        ordinal++;
        continue;
      }
      const bool folder = BKE_paint_layers_is_folder(correction);
      StackRow row;
      row.ordinal = int16_t(ordinal++);
      row.depth = parent_depth + 1;
      row.parent_ordinal = int16_t(parent_ordinal);
      row.parent_section_id = (role == MA_PAINT_LAYER_ROLE_MASK_ITEM) ? "MASK" : "CHANNELS";
      row.stable_id = correction.marker;
      row.enabled = (correction.flag & MA_PAINT_LAYER_ENABLED) != 0;
      row.supported = true;
      row.name = correction.name[0] != '\0' ? correction.name :
                 folder                        ? "Folder" :
                                                 "Correction";
      row.name_buffer = const_cast<char *>(correction.name);
      row.can_hold_children = folder;
      row.has_children = folder && !BLI_listbase_is_empty(&correction.children);
      /* Every correction is a dense setting under its layer, not a stack member of its own: it
       * keeps the usual row height and shows what it is through a small icon instead of a preview.
       * A mask item reads as a mask and carries the state icon a masked Layer row does; a content
       * effect takes the icon of its source. */
      row.compact = true;
      if (folder) {
        row.icon = ICON_FILE_FOLDER;
      }
      else if (role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
        row.icon = paint_mask_state_icon(row.enabled);
      }
      else {
        switch (correction.source) {
          case MA_PAINT_LAYER_SOURCE_CONSTANT:
            row.icon = ICON_GP_DRAW_FILL;
            break;
          case MA_PAINT_LAYER_SOURCE_MATERIAL:
            row.icon = ICON_MATERIAL;
            break;
          case MA_PAINT_LAYER_SOURCE_NODE_GROUP:
            row.icon = ICON_NODETREE;
            break;
          case MA_PAINT_LAYER_SOURCE_MESH_MAP:
            row.icon = ICON_MESH_DATA;
            break;
          default:
            row.icon = ICON_SCULPTMODE_HLT;
            break;
        }
        /* A compact row draws no preview slots, so a refused source is shown by the icon itself. */
        PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
        if (correction.source == MA_PAINT_LAYER_SOURCE_MATERIAL &&
            BKE_paint_layers_material_live_status(material, correction, &refusal) ==
                PaintLayerMaterialLiveStatus::Refused)
        {
          row.icon = ICON_ERROR;
        }
      }
      append_issue_slots(row, layer.marker, correction.marker);
      set_pair_columns(row, correction);
      const int row_ordinal = row.ordinal;
      if (!folder) {
        r_rows.append(std::move(row));
      }
      if (folder) {
        /* Depth/parent bookkeeping mirrors a folder Layer's own recursion below: the correction's
         * row is at `parent_depth + 1`, so its children sit one further in. */
        append_list(correction.children, parent_depth + 2, row_ordinal);
        r_rows.append(std::move(row));
      }
    }
  };

  append_list =
      [&](const ListBaseT<MaterialPaintLayer> &list, const int depth, const int parent_ordinal) {
        for (const MaterialPaintLayer &layer : list) {
          if (overflow) {
            return;
          }
          if (ordinal > STACK_ROW_ORDINAL_MAX) {
            overflow = true;
            return;
          }
          StackRow row;
          row.ordinal = int16_t(ordinal++);
          row.depth = depth;
          row.parent_ordinal = int16_t(parent_ordinal);
          row.stable_id = layer.marker;
          const bool folder = BKE_paint_layers_is_folder(layer);
          row.can_hold_children = folder;
          row.has_children = folder && !BLI_listbase_is_empty(&layer.children);
          row.enabled = (layer.flag & MA_PAINT_LAYER_ENABLED) != 0;
          /* The row's mask slot stands for the base mask, so its presence and its on/off state
           * come from that item, not from whichever correction happens to be enabled. */
          const MaterialPaintLayer *mask_base = BKE_paint_layers_mask_base(layer);
          const bool has_mask = mask_base != nullptr;
          row.mask_enabled = !has_mask || ((mask_base->flag & MA_PAINT_LAYER_ENABLED) != 0);
          row.supported = true;
          row.name = layer.name[0] != '\0' ? layer.name :
                     folder                      ? "Folder" :
                                                   "Layer";
          row.name_buffer = const_cast<char *>(layer.name);
          row.color_tag = layer.color_tag;
          row.icon = folder                               ? ICON_FILE_FOLDER :
                     layer.source == MA_PAINT_LAYER_SOURCE_CONSTANT ? ICON_GP_DRAW_FILL :
                     has_mask                           ? paint_mask_state_icon(row.mask_enabled) :
                                                          ICON_IMAGE_RGB;

          if (!folder) {
            if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer.material != nullptr) {
              /* A Material layer holds no channel image of its own -- its data lives in the source
               * node group -- so the slot shows the material's own preview rather than the empty
               * first channel, which would otherwise draw as a blank placeholder. */
              StackRowPreview channels_slot;
              channels_slot.section_id = "CHANNELS";
              channels_slot.id_uid = layer.material->id.session_uid;
              channels_slot.id_type = ID_MA;
              channels_slot.is_blank = false;
              row.preview_slots.append(std::move(channels_slot));
            }
            else if (layer.source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
              /* A Fill is its colour: one swatch at preview size with the bucket drawn over it, and
               * no channel-texture slot -- the first channel is a generated map that would only add
               * an empty-texture placeholder next to the colour. Clicking the swatch activates the
               * layer and its channel section, and opens the picker. With the Base Color record
               * removed or switched off the row paints no colour, so no swatch is drawn: the row's
               * own fill icon stands alone instead of a colour the layer no longer lays. */
              if (BKE_paint_layers_base_color_active(layer)) {
                StackRowPreview fill_swatch;
                fill_swatch.is_color_swatch = true;
                fill_swatch.icon = ICON_GP_DRAW_FILL;
                fill_swatch.section_id = "CHANNELS";
                BKE_paint_layers_base_color_get(layer, fill_swatch.color);
                fill_swatch.label = IFACE_("Fill Color");
                row.preview_slots.append(std::move(fill_swatch));
              }
            }
            else {
              StackRowPreview channels_slot;
              channels_slot.section_id = "CHANNELS";
              for (int c = 0; c < layer.channels_num; c++) {
                /* A channel outside the material's set is not shown: it takes part nowhere. */
                if (!BKE_paint_layers_channel_in_set(
                        material, eMaterialPaintChannel(layer.channels[c].channel)))
                {
                  continue;
                }
                const Image *image = layer.channels[c].image;
                if (image == nullptr) {
                  continue;
                }
                channels_slot.id_uid = image->id.session_uid;
                channels_slot.id_type = ID_IM;
                channels_slot.is_blank = paint_image_is_blank(*image);
                break;
              }
              row.preview_slots.append(std::move(channels_slot));
            }
            if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
              append_material_refusal_slot(row, layer);
            }

            StackContentSection channels;
            channels.identifier = "CHANNELS";
            channels.name = IFACE_("Channel Textures");
            /* The channel maps are the layer's hidden internals, not rows of the stack: the
             * section stays so the corrections attach to it and the brush can switch to the
             * content, but it lists no map sub-rows. */
            row.content_sections.append(std::move(channels));
          }

          if (folder) {
            /* A folder's content is the layers it holds and the corrections hung on it: its own
             * section, so a mask can be switched to and back, and the corrections attached to it
             * have a section to be listed under. */
            StackContentSection content;
            content.identifier = "CHANNELS";
            content.name = IFACE_("Content");
            row.content_sections.append(std::move(content));
          }

          if (has_mask) {
            if (folder) {
              /* The slot that switches back to the content, ahead of the mask's. The folder icon
               * stays in front of it, as it does for a folder with only a mask. */
              StackRowPreview content_slot;
              content_slot.section_id = "CHANNELS";
              content_slot.icon = ICON_BRUSHES_ALL;
              content_slot.keeps_row_icon = true;
              content_slot.label = IFACE_("Content");
              row.preview_slots.append(std::move(content_slot));
            }
            row.preview_slots.append(paint_mask_slot_build(false, row.mask_enabled));
            /* After Channel Textures: the mask section is what a click switches the brush to. */
            row.content_sections.append(paint_mask_section_build(material, layer));
          }

          /* The columns edit the pair of the selected channel: its record's RNA when the pair has
           * one, inherited controls -- which create the record on first edit -- when it has not. */
          set_pair_columns(row, layer);

          append_issue_slots(row, layer.marker, {});
          const int row_ordinal = row.ordinal;
          if (!folder) {
            r_rows.append(std::move(row));
          }

          append_corrections(layer, row_ordinal, depth, MA_PAINT_LAYER_ROLE_EFFECT);
          append_corrections(layer, row_ordinal, depth, MA_PAINT_LAYER_ROLE_MASK_ITEM);
          if (folder) {
            append_list(layer.children, depth + 1, row_ordinal);
            /* The tree is built walking the rows from the last one down, and a child is only hung
             * off a folder already made; so a folder's row follows its contents in the vector.
             * The ordinals keep the walk order above -- rows are looked up by ordinal, never by
             * position. */
            r_rows.append(std::move(row));
          }
        }
      };

  append_list(material.paint_layers, 0, -1);
  if (overflow) {
    StackRow row;
    row.ordinal = STACK_ROW_ORDINAL_MAX + 1;
    row.supported = false;
    row.unsupported_reason = "Stack is too large to display";
    row.icon = ICON_IMAGE_RGB;
    r_rows.append(std::move(row));
  }
}
}  // namespace


/* -------------------------------------------------------------------- */
/** \name Context-free description edits
 *
 * The bodies of #PaintLayersStackSource's verbs, kept free of a #bContext so they can be unit
 * tested: each one performs the BKE edit and reports what it did. The verbs add the reports and
 * notifiers on top.
 * \{ */

namespace {
/** The Add policy lives in BKE (#BKE_paint_layers_add_with_policy); this only translates the
 * source's Add vocabulary and the anchor ordinal into the BKE call's terms. */
PaintLayerAddKind stack_add_kind_to_paint_kind(const int kind)
{
  switch (kind) {
    case PAINT_STACK_ADD_PAINT:
      return PaintLayerAddKind::Paint;
    case PAINT_STACK_ADD_FILL:
      return PaintLayerAddKind::Fill;
    case PAINT_STACK_ADD_MATERIAL:
      return PaintLayerAddKind::Material;
    case PAINT_STACK_ADD_FOLDER:
      return PaintLayerAddKind::Folder;
    case PAINT_STACK_ADD_CORRECTION_PAINT:
      return PaintLayerAddKind::EffectPaint;
    case PAINT_STACK_ADD_CORRECTION_FILL:
      return PaintLayerAddKind::EffectFill;
    case PAINT_STACK_ADD_CORRECTION_MESH_MAP:
      return PaintLayerAddKind::EffectMeshMap;
    case PAINT_STACK_ADD_CORRECTION_MATERIAL:
      return PaintLayerAddKind::EffectMaterial;
    case PAINT_STACK_ADD_CORRECTION_NODE_GROUP:
      return PaintLayerAddKind::EffectNodeGroup;
    case PAINT_STACK_ADD_CORRECTION_STACK:
      return PaintLayerAddKind::EffectStack;
    case PAINT_STACK_ADD_MASK_CORRECTION_PAINT:
      return PaintLayerAddKind::MaskPaint;
    case PAINT_STACK_ADD_MASK_CORRECTION_FILL:
      return PaintLayerAddKind::MaskFill;
    case PAINT_STACK_ADD_MASK_CORRECTION_MESH_MAP:
      return PaintLayerAddKind::MaskMeshMap;
    case PAINT_STACK_ADD_MASK_CORRECTION_MATERIAL:
      return PaintLayerAddKind::MaskMaterial;
    case PAINT_STACK_ADD_MASK_CORRECTION_NODE_GROUP:
      return PaintLayerAddKind::MaskNodeGroup;
    case PAINT_STACK_ADD_MASK_CORRECTION_STACK:
      return PaintLayerAddKind::MaskStack;
  }
  return PaintLayerAddKind::Paint;
}
}  // namespace

int paint_layers_edit_add(Material &material,
                          const int kind,
                          const int ordinal,
                          const StackAddArgs &args)
{
  MaterialPaintLayer *anchor = (ordinal < 0) ? nullptr :
                                                paint_description_row_for_ordinal(material, ordinal);
  if (ordinal >= 0 && anchor == nullptr) {
    return -1;
  }
  PaintLayerAddParams params;
  params.kind = stack_add_kind_to_paint_kind(kind);
  params.anchor = anchor;
  params.source = args.source;
  params.fill_color = args.color;
  MaterialPaintLayer *created = BKE_paint_layers_add_with_policy(material, params);
  return (created != nullptr) ? layers_ordinal_of(material, created) : -1;
}

bool paint_layers_edit_remove(Material &material, const int ordinal)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  return layer != nullptr && BKE_paint_layers_remove(material, layer);
}

bool paint_layers_edit_move(Material &material,
                            const int from_ordinal,
                            const int anchor_ordinal,
                            const StackMovePlace place,
                            int *r_ordinal)
{
  MaterialPaintLayer *from = paint_description_row_for_ordinal(material, from_ordinal);
  MaterialPaintLayer *anchor = (anchor_ordinal < 0) ?
                                   nullptr :
                                   paint_description_row_for_ordinal(material, anchor_ordinal);
  if (from == nullptr || (anchor_ordinal >= 0 && anchor == nullptr)) {
    return false;
  }
  const PaintLayerPlace layer_place = (place == StackMovePlace::Below) ? PaintLayerPlace::Below :
                                      (place == StackMovePlace::Into) ? PaintLayerPlace::Into :
                                                                        PaintLayerPlace::Above;
  if (!BKE_paint_layers_edit_move(material, *from, anchor, layer_place)) {
    return false;
  }
  if (r_ordinal != nullptr) {
    *r_ordinal = layers_ordinal_of(material, from);
  }
  return true;
}

bool paint_layers_edit_set_enabled(Material &material, const int ordinal, const bool enable)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  return layer != nullptr && BKE_paint_layers_set_enabled(material, layer, enable);
}

int paint_layers_edit_duplicate(Main &bmain, Material &material, const int ordinal)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  if (layer == nullptr) {
    return -1;
  }
  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(bmain, material, layer);
  return (copy != nullptr) ? layers_ordinal_of(material, copy) : -1;
}

bool paint_layers_edit_rename(Material &material, const int ordinal, const char *name)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  return layer != nullptr && BKE_paint_layers_rename(material, layer, name);
}

bool paint_layers_edit_mask_set(Material &material, const int ordinal, const bool add)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  return layer != nullptr && BKE_paint_layers_edit_mask_set(material, *layer, add);
}

bool paint_layers_edit_mask_toggle(Material &material, const int ordinal)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  if (layer == nullptr) {
    return false;
  }
  /* The layer's mask is its base item. */
  MaterialPaintLayer *item = BKE_paint_layers_mask_base(*layer);
  if (item == nullptr) {
    return false;
  }
  const bool enable = (item->flag & MA_PAINT_LAYER_ENABLED) == 0;
  return BKE_paint_layers_set_enabled(material, item, enable);
}

int paint_layers_edit_merge_down(Material &material, const int ordinal)
{
  MaterialPaintLayer *upper = paint_description_row_for_ordinal(material, ordinal);
  MaterialPaintLayer *lower = (ordinal > 0) ?
                                  paint_description_row_for_ordinal(material, ordinal - 1) :
                                  nullptr;
  if (upper == nullptr || lower == nullptr) {
    return -1;
  }
  MaterialPaintLayer *folder = BKE_paint_layers_edit_merge_down(material, *upper, *lower);
  return (folder != nullptr) ? layers_ordinal_of(material, folder) : -1;
}

int paint_layers_edit_group_range(Material &material, const int from_ordinal, const int to_ordinal)
{
  const int low = std::min(from_ordinal, to_ordinal);
  const int high = std::max(from_ordinal, to_ordinal);
  if (low < 0 || high < low) {
    return -1;
  }
  Vector<MaterialPaintLayer *> to_group;
  for (int ordinal = low; ordinal <= high; ordinal++) {
    MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
    if (layer == nullptr) {
      return -1;
    }
    to_group.append(layer);
  }
  MaterialPaintLayer *folder = BKE_paint_layers_edit_group_range(material, to_group);
  return (folder != nullptr) ? layers_ordinal_of(material, folder) : -1;
}

int paint_layers_edit_ungroup(Material &material, const int ordinal)
{
  MaterialPaintLayer *folder = paint_description_row_for_ordinal(material, ordinal);
  if (folder == nullptr || !BKE_paint_layers_is_folder(*folder)) {
    return -1;
  }
  const int child_num = BLI_listbase_count(&folder->children);
  return BKE_paint_layers_ungroup(material, folder) ? child_num : -1;
}

int paint_layers_edit_group_add(Material &material, const int ordinal)
{
  MaterialPaintLayer *anchor = (ordinal < 0) ? nullptr :
                                                paint_description_row_for_ordinal(material, ordinal);
  if (ordinal >= 0 && anchor == nullptr) {
    return -1;
  }
  MaterialPaintLayer *folder = BKE_paint_layers_edit_group_add(material, anchor);
  return (folder != nullptr) ? layers_ordinal_of(material, folder) : -1;
}

bool paint_layers_edit_color_tag(Material &material, const int ordinal, const int color_tag)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  return layer != nullptr && BKE_paint_layers_set_color_tag(material, layer, int8_t(color_tag));
}

bool paint_layers_edit_fill_color(Material &material, const int ordinal, const float color[4])
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  return layer != nullptr && layer->source == MA_PAINT_LAYER_SOURCE_CONSTANT &&
         BKE_paint_layers_set_fill_color(material, layer, color);
}

bool paint_layers_edit_reorder(Material &material, const int from_ordinal, const int to_ordinal)
{
  MaterialPaintLayer *from = paint_description_row_for_ordinal(material, from_ordinal);
  MaterialPaintLayer *to = paint_description_row_for_ordinal(material, to_ordinal);
  if (from == nullptr || to == nullptr) {
    return false;
  }
  return BKE_paint_layers_edit_reorder(material, *from, *to);
}

/** \} */

/**
 * The layered-material half of the paint stack source: rows and every verb come from the DNA
 * description, addressed by marker. It is stateless; #PaintMaterialStackSource forwards to a
 * shared instance of it.
 */
class PaintLayersStackSource final : public StackSource,
                                     public StackEditor,
                                     public StackGroupingEditor,
                                     public StackColorEditor,
                                     public StackDropHandler {
 public:
  eSpaceOutliner_StackSource type() const override
  {
    return SO_STACK_SRC_PAINT_MATERIAL;
  }

  StringRefNull ui_name() const override
  {
    return N_("Paint Layers");
  }

  bool object_has_stack(const StackReadContext & /*ctx*/, Object &object) const override
  {
    const Material *material = BKE_object_material_get(&object, object.actcol);
    return material != nullptr && paint_layers_is_layered(*material);
  }

  ID *object_preview_id(const StackReadContext & /*ctx*/, Object &object) const override
  {
    Material *material = BKE_object_material_get(&object, object.actcol);
    return (material != nullptr) ? &material->id : nullptr;
  }

  ID *owner_get(const StackReadContext &ctx, const StackFocus &focus) const override
  {
    Object *object = outliner_stack_focus_object_get(ctx, focus);
    if (object == nullptr) {
      return nullptr;
    }
    const short slot = focus.sub_index >= 0 ? short(focus.sub_index + 1) : object->actcol;
    Material *material = BKE_object_material_get(object, slot);
    return (material != nullptr) ? &material->id : nullptr;
  }

  void sub_selections_get(const StackReadContext &ctx,
                          const StackFocus &focus,
                          Vector<StackSubSelection> &r_items) const override
  {
    Object *object = outliner_stack_focus_object_get(ctx, focus);
    if (object == nullptr) {
      return;
    }
    for (const int slot : IndexRange(object->totcol)) {
      const Material *material = BKE_object_material_get(object, short(slot + 1));
      StackSubSelection item;
      item.name = material != nullptr ? material->id.name + 2 : "";
      item.preview_id = (material != nullptr) ? &const_cast<Material *>(material)->id : nullptr;
      r_items.append(std::move(item));
    }
  }

  void sub_selection_apply(bContext & /*C*/,
                           const StackFocus &focus,
                           Object &object) const override
  {
    if (focus.sub_index >= 0 && focus.sub_index < object.totcol) {
      object.actcol = short(focus.sub_index + 1);
    }
  }

  uint64_t state_hash(const StackReadContext &ctx, const ID &owner) const override
  {
    const Material &material = layers_owner(owner);
    uint64_t hash = uint64_t(material.paint_layers_flag);
    hash = hash * 1000003u ^ uint64_t(paint_stack_selected_channel(ctx));
    std::function<void(const ListBaseT<MaterialPaintLayer> &)> walk =
        [&](const ListBaseT<MaterialPaintLayer> &list) {
          for (const MaterialPaintLayer &layer : list) {
        hash = hash * 1000003u ^ UUID(layer.marker).hash();
        hash ^= uint64_t(layer.source) | (uint64_t(layer.flag) << 8) |
                (uint64_t(layer.blend) << 16) | (uint64_t(layer.mesh_map_type) << 24);
        /* The swatch and the columns show these, so a change to them has to reach the rows. The
         * Base Color comes from the single reader, so a Layer-role Fill (whose colour lives in its
         * Base-Color record) invalidates exactly as a mask or Fill-effect correction (DNA field)
         * does. */
        float base_color[4];
        BKE_paint_layers_base_color_get(layer, base_color);
        for (const float value : {base_color[0],
                                  base_color[1],
                                  base_color[2],
                                  base_color[3],
                                  layer.opacity})
        {
          hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(value));
        }
        for (const MaterialPaintLayerChannelSettings &settings : layer.channel_settings) {
          hash = hash * 1000003u ^ uint64_t(uint8_t(settings.blend));
          hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(settings.opacity));
        }
        for (const char *ch = layer.name; *ch != '\0'; ch++) {
          hash = hash * 131u ^ uint64_t(uint8_t(*ch));
        }
        for (int c = 0; c < layer.channels_num; c++) {
          const Image *image = layer.channels[c].image;
          hash ^= uint64_t(layer.channels[c].channel) << 32;
          hash ^= uint64_t(layer.channels[c].state) << 40;
          if (image != nullptr) {
            hash ^= uint64_t(image->id.session_uid) * 2654435761u;
            /* A blank generated map is drawn as a placeholder; the first stroke that touches it
             * flips this, so the rows (and their previews) must rebuild. */
            hash = hash * 1000003u ^ uint64_t(paint_image_is_blank(*image) ? 1 : 0);
          }
        }
        if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL &&
            BKE_paint_layers_source_group_build_failed_get(material, layer))
        {
          /* A build failure is runtime data, not in `paint_layers_flag`: without mixing it in the
           * cached rows would miss the "Refused" icon after the wrapper failed to build. */
          hash = hash * 1000003u ^ 0xB17D0FFAULL;
        }
        auto hash_corrections = [&](const ListBaseT<MaterialPaintLayer> &corrections) {
          for (const MaterialPaintLayer &correction : corrections) {
            hash = hash * 1000003u ^ UUID(correction.marker).hash();
            hash ^= uint64_t(correction.source) | (uint64_t(correction.flag) << 8) |
                    (uint64_t(correction.role) << 16) |
                    (uint64_t(correction.mesh_map_type) << 24);
            for (int c = 0; c < correction.channels_num; c++) {
              const Image *image = correction.channels[c].image;
              hash ^= uint64_t(correction.channels[c].channel) << 32;
              hash ^= uint64_t(correction.channels[c].state) << 40;
              if (image != nullptr) {
                hash ^= uint64_t(image->id.session_uid) * 2654435761u;
                hash = hash * 1000003u ^ uint64_t(paint_image_is_blank(*image) ? 1 : 0);
              }
            }
            /* A Stack-sourced correction/mask item is a folder like any other (Phase 6, goal 4):
             * an edit to a row inside it must invalidate the cached rows exactly as an edit inside a
             * folder Layer's own children already does via #walk below. */
            if (BKE_paint_layers_is_folder(correction)) {
              walk(correction.children);
            }
          }
        };
        hash_corrections(layer.effects);
        hash_corrections(layer.mask_stack);
        walk(layer.children);
      }
    };
    walk(material.paint_layers);
    return hash;
  }

  bool rows_build(const StackReadContext &ctx,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  Vector<StackRow> &r_rows) const override
  {
    paint_stack_rows_from_description(
        layers_owner(owner), paint_stack_selected_channel(ctx), r_rows);
    return !r_rows.is_empty();
  }

  bool is_editable(const ID &owner) const override
  {
    const Material &material = layers_owner(owner);
    return ID_IS_EDITABLE(&material.id) && !ID_IS_OVERRIDE_LIBRARY(&material.id);
  }

  StackColumnLayout column_layout() const override
  {
    /* Wide enough for the compact row's "NN% Abr" label to fit without clipping (the label is
     * shown in the stacked column, whose width is the wider of the two). */
    return {2.6f, 2.6f};
  }

  bool row_activate(bContext &C,
                    const StackFocus & /*focus*/,
                    ID &owner,
                    const int /*ordinal*/,
                    const StackRow &row) const override
  {
    Material &material = layers_owner(owner);
    if (!this->is_editable(owner)) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Layered material is not editable");
      return false;
    }
    if (!row.supported) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Unsupported layer cannot be activated");
      return false;
    }
    BKE_paint_layers_active_set(material, row.stable_id);
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
    return true;
  }

  bool row_is_active(const StackReadContext & /*ctx*/,
                     const StackFocus & /*focus*/,
                     const ID &owner,
                     const StackRow &row) const override
  {
    const Material &material = layers_owner(owner);
    return !BLI_uuid_is_nil(row.stable_id) &&
           BLI_uuid_equal(material.active_layer_marker, row.stable_id);
  }

  bool sub_row_activate(bContext &C,
                        const StackFocus & /*focus*/,
                        ID &owner,
                        const StackRow & /*row*/,
                        const StackSubRow &sub_row) const override
  {
    const int role = layers_channel_role_channel(sub_row);
    Scene *scene = CTX_data_scene(&C);
    /* A channel sub-row names the channel the next stroke paints, whether or not it has a map
     * yet: an empty channel is exactly the one a first stroke should create. */
    if (scene != nullptr && scene->toolsettings != nullptr &&
         role >= 0 && role < PAINT_MATERIAL_CHANNEL_NUM)
    {
      scene->toolsettings->paint_mode.active_layer_channel = role;
      Material &material = layers_owner(owner);
      material.paint_layers_flag |= MA_PAINT_LAYERS_SLOTS_STALE;
      WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
    }
    if (sub_row.id == nullptr) {
      return true;
    }
    return paint_layers_image_open(C, sub_row.id);
  }

  bool preview_activate(bContext &C,
                        ID & /*owner*/,
                        const StackRow &row,
                        const StringRef section_id) const override
  {
    bool has_section = false;
    for (const StackContentSection &section : row.content_sections) {
      has_section |= section.identifier == section_id;
    }
    if (!has_section) {
      return false;
    }
    if (section_id == "MASK") {
      return layers_target_mode_set(C, PAINT_LAYER_TARGET_MASK);
    }
    if (section_id == "CHANNELS") {
      return layers_target_mode_set(C, PAINT_LAYER_TARGET_CONTENT);
    }
    return false;
  }

  bool target_clear(bContext & /*C*/) const override
  {
    return false;
  }

  std::optional<eObjectMode> focus_object_mode() const override
  {
    return OB_MODE_TEXTURE_PAINT;
  }

  bool notifier_invalidates(const wmNotifier &notifier) const override
  {
    if (notifier.category == NC_MATERIAL) {
      return true;
    }
    /* The Stack Layers channel selection changes which pair the columns address. */
    if (notifier.category == NC_SPACE && notifier.data == ND_SPACE_OUTLINER) {
      return true;
    }
    /* A first stroke or a dropped map materializes an image the rows list. */
    if (notifier.category == NC_IMAGE) {
      return ELEM(notifier.action, NA_ADDED, NA_REMOVED, NA_EDITED);
    }
    return false;
  }

  bool focus_will_open(bContext & /*C*/, ID & /*owner*/) const override
  {
    /* The description already carries markers and needs no normalization. */
    return false;
  }

  const StackEditor *editor() const override
  {
    return this;
  }

  const StackGroupingEditor *grouping() const override
  {
    return this;
  }

  const StackColorEditor *color() const override
  {
    return this;
  }

  const StackDropHandler *drop_handler() const override
  {
    return this;
  }

  /* ---------------------------------------------------------------- */
  /** \name StackEditor
   * \{ */

  bool can_reorder(const ID &owner) const override
  {
    return this->is_editable(owner);
  }

  bool row_reorder(bContext &C,
                   const StackFocus & /*focus*/,
                   ID &owner,
                   const int from_ordinal,
                   const int to_ordinal) const override
  {
    Material &material = layers_owner(owner);
    if (!paint_layers_edit_reorder(material, from_ordinal, to_ordinal)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  void add_kinds(Vector<StackAddKindInfo> &r_kinds) const override
  {
    /* Indices are the shared paint list's; this source only reads the ones it handles. Kept in
     * step with PaintMaterialStackSource::add_kinds -- the list is one Add UI for both truths. */
    r_kinds.append({"PAINT",
                    "Paint Layer",
                    "A layer that shows nothing until it is painted on",
                    ICON_BRUSH_DATA});
    StackAddKindInfo fill{"FILL",
                          "Fill Layer",
                          "A layer of one flat colour, revealed by its mask",
                          ICON_GP_DRAW_FILL};
    fill.takes_color = true;
    r_kinds.append(fill);
    StackAddKindInfo material_kind{"MATERIAL",
                                   "Material Layer",
                                   "A material baked into the layer's maps",
                                   ICON_MATERIAL};
    /* A Material layer is made from a chosen material: the Add asks for one and hands it in as the
     * source. */
    material_kind.source_id_type = ID_MA;
    r_kinds.append(material_kind);
    r_kinds.append({"FOLDER", IFACE_("Folder"), "A group holding other layers", ICON_FILE_FOLDER});
    r_kinds.append({"CORRECTION_PAINT",
                    IFACE_("Paint"),
                    "A painted adjustment hung on the content of the row this is added from",
                    ICON_BRUSH_DATA});
    r_kinds.append({"CORRECTION_FILL",
                    IFACE_("Fill"),
                    "A flat-fill adjustment on the content of the row this is added from",
                    ICON_BRUSH_DATA});
    r_kinds.append({"MASK_CORRECTION_PAINT",
                    IFACE_("Mask Correction"),
                    "A painted correction limiting where the row this is added from applies",
                    ICON_BRUSH_DATA});
    r_kinds.append({"MASK_CORRECTION_FILL",
                    IFACE_("Mask Fill Correction"),
                    "A flat-fill correction limiting where the row this is added from applies",
                    ICON_BRUSH_DATA});

    /* Phase 6, goal 1: the remaining Layer sources, offered as a correction/mask the same way
     * Paint/Fill already are. Order matches #PaintStackAddKind exactly (index-based dispatch). */
    r_kinds.append({"CORRECTION_MESH_MAP",
                    IFACE_("Mesh Map Correction"),
                    "A geometry map of the object adjusting the content of the row this is added "
                    "from",
                    ICON_BRUSH_DATA});
    StackAddKindInfo correction_material{
        "CORRECTION_MATERIAL",
        IFACE_("Material Correction"),
        "Another material's channels, baked, adjusting the content of the row this is added from",
        ICON_MATERIAL};
    correction_material.source_id_type = ID_MA;
    r_kinds.append(correction_material);
    StackAddKindInfo correction_node_group{
        "CORRECTION_NODE_GROUP",
        IFACE_("Node Group Correction"),
        "A user's node group adjusting the content of the row this is added from",
        ICON_NODETREE};
    correction_node_group.source_id_type = ID_NT;
    r_kinds.append(correction_node_group);
    r_kinds.append({"CORRECTION_STACK",
                    IFACE_("Folder Correction"),
                    "A nested stack of layers adjusting the content of the row this is added from",
                    ICON_FILE_FOLDER});

    r_kinds.append({"MASK_CORRECTION_MESH_MAP",
                    IFACE_("Mesh Map Mask"),
                    "A geometry map of the object limiting where the row this is added from "
                    "applies",
                    ICON_BRUSH_DATA});
    StackAddKindInfo mask_correction_material{
        "MASK_CORRECTION_MATERIAL",
        IFACE_("Material Mask"),
        "Another material's channels, baked, limiting where the row this is added from applies",
        ICON_MATERIAL};
    mask_correction_material.source_id_type = ID_MA;
    r_kinds.append(mask_correction_material);
    StackAddKindInfo mask_correction_node_group{
        "MASK_CORRECTION_NODE_GROUP",
        IFACE_("Node Group Mask"),
        "A user's node group limiting where the row this is added from applies",
        ICON_NODETREE};
    mask_correction_node_group.source_id_type = ID_NT;
    r_kinds.append(mask_correction_node_group);
    r_kinds.append({"MASK_CORRECTION_STACK",
                    IFACE_("Folder Mask"),
                    "A nested stack of layers limiting where the row this is added from applies",
                    ICON_FILE_FOLDER});
  }

  int row_add(bContext &C,
              const StackFocus & /*focus*/,
              ID &owner,
              const int kind,
              const int ordinal,
              const StackAddArgs &args = {}) const override
  {
    if (!this->is_editable(owner)) {
      return -1;
    }
    Material &material = layers_owner(owner);
    /* A stack that names no UV layer adopts the active object's active one on the first add, so the
     * generated graph is wired to the layer the user is unwrapped with. */
    BKE_paint_layers_uv_map_autofill(material, CTX_data_active_object(&C));
    if (kind == PAINT_STACK_ADD_MATERIAL) {
      /* The Material layer bakes its source, so it is made by the same ED verb the material drop
       * uses; that owns the bake and the reports. */
      Material *source = (args.source != nullptr && GS(args.source->name) == ID_MA) ?
                             id_cast<Material *>(args.source) :
                             nullptr;
      ReportList *reports = CTX_wm_reports(&C);
      if (source == nullptr) {
        BKE_report(reports, RPT_ERROR, "Choose a material to bake from");
        return -1;
      }
      /* Self and cycle sources are refused by #add_material_layer_from_material itself: one
       * report path for every authoring entry. */
      MaterialPaintLayer *anchor = (ordinal < 0) ?
                                       nullptr :
                                       paint_description_row_for_ordinal(material, ordinal);
      const PaintLayerPlace place = (anchor != nullptr && BKE_paint_layers_is_folder(*anchor)) ?
                                        PaintLayerPlace::Into :
                                        PaintLayerPlace::Above;
      MaterialPaintLayer *layer =
          ed::sculpt_paint::material_layer::add_material_layer_from_material(
              C, material, *source, anchor, place);
      if (layer == nullptr) {
        return -1;
      }
      BKE_paint_layers_active_set(material, layer->marker);
      /* A new layer is painted on its content, not on the mask the previous layer was edited in. */
      layers_target_mode_set(C, PAINT_LAYER_TARGET_CONTENT);
      WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
      WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
      return layers_ordinal_of(material, layer);
    }
    const int created = paint_layers_edit_add(material, kind, ordinal, args);
    if (created < 0) {
      return -1;
    }
    /* A painted mask correction gets its opaque map now, neutral for its blend, rather than on
     * the first stroke: the neutral depends on the mode, and the mode may change after the map
     * exists. A Fill mask correction reads its constant until phase 4, so it grows no map here. */
    if (kind == PAINT_STACK_ADD_MASK_CORRECTION_PAINT) {
      if (MaterialPaintLayer *correction = paint_description_row_for_ordinal(material, created)) {
        if (BKE_paint_layers_role(*correction) == PaintLayerRole::MaskItem) {
          Main *bmain = CTX_data_main(&C);
          if (bmain != nullptr) {
            /* A painted mask item gets its map now, so an unpainted item changes nothing. */
            PaintLayersTarget target;
            target.material = &material;
            target.layer = correction;
            target.mask_item = correction;
            target.mode = PaintLayersTargetMode::Mask;
            Scene *scene = CTX_data_scene(&C);
            const int size = (scene != nullptr && scene->toolsettings != nullptr &&
                              scene->toolsettings->paint_mode.new_channel_image_size > 0) ?
                                 scene->toolsettings->paint_mode.new_channel_image_size :
                                 1024;
            BKE_paint_layers_target_ensure_writable(*bmain, target, size, nullptr);
          }
        }
      }
    }
    /* A mask the user just added is the one they mean to work on: the caller activates its row, and
     * the brush has to target the mask rather than keep painting the layer's content. */
    if (ELEM(kind,
             PAINT_STACK_ADD_MASK_CORRECTION_PAINT,
             PAINT_STACK_ADD_MASK_CORRECTION_FILL,
             PAINT_STACK_ADD_MASK_CORRECTION_MESH_MAP,
             PAINT_STACK_ADD_MASK_CORRECTION_MATERIAL,
             PAINT_STACK_ADD_MASK_CORRECTION_NODE_GROUP,
             PAINT_STACK_ADD_MASK_CORRECTION_STACK))
    {
      layers_target_mode_set(C, PAINT_LAYER_TARGET_MASK);
    }
    else {
      /* A new layer is painted on its content, not on the mask the previous layer was edited in:
       * the target mode is global, so it would otherwise carry over and give the layer a mask. */
      layers_target_mode_set(C, PAINT_LAYER_TARGET_CONTENT);
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return created;
  }

  bool row_set_enabled(bContext &C,
                       const StackFocus & /*focus*/,
                       ID &owner,
                       const int ordinal,
                       const bool enable) const override
  {
    Material &material = layers_owner(owner);
    if (!paint_layers_edit_set_enabled(material, ordinal, enable)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  int row_duplicate(bContext &C,
                    const StackFocus & /*focus*/,
                    ID &owner,
                    const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    Main *bmain = CTX_data_main(&C);
    if (bmain == nullptr) {
      return -1;
    }
    const int copy = paint_layers_edit_duplicate(*bmain, material, ordinal);
    if (copy < 0) {
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return copy;
  }

  bool row_rename(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal,
                  const StringRefNull name) const override
  {
    Material &material = layers_owner(owner);
    if (!paint_layers_edit_rename(material, ordinal, name.c_str())) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Cannot rename this row");
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool row_remove(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
    if (layer == nullptr) {
      return false;
    }
    sculpt_paint::material_layer::mask_edit_end_if_target_removed(C, material, layer->marker);
    if (!paint_layers_edit_remove(material, ordinal)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool row_move(bContext &C,
                const StackFocus & /*focus*/,
                ID &owner,
                const int from_ordinal,
                const int anchor_ordinal,
                const StackMovePlace place,
                int *r_ordinal) const override
  {
    Material &material = layers_owner(owner);
    if (!paint_layers_edit_move(material, from_ordinal, anchor_ordinal, place, r_ordinal)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  /**
   * The effect or mask item \a source names, in whichever material it lives: a copy carries across
   * materials, so the clipboard may still hold a row of a stack the user has since left.
   */
  static const MaterialPaintLayer *paste_source_row(Main &bmain, const StackItemIdentity &source)
  {
    if (source.source_type != SO_STACK_SRC_PAINT_MATERIAL || BLI_uuid_is_nil(source.row_id)) {
      return nullptr;
    }
    for (Material *ma = static_cast<Material *>(bmain.materials.first); ma != nullptr;
         ma = static_cast<Material *>(ma->id.next))
    {
      if (ma->id.session_uid == source.owner_uid) {
        return BKE_paint_layers_find(*ma, source.row_id);
      }
    }
    return nullptr;
  }

  bool can_paste_into(const StackReadContext &ctx,
                      const StackFocus & /*focus*/,
                      const ID &owner,
                      const StackItemIdentity &source,
                      const int target_ordinal) const override
  {
    if (ctx.bmain == nullptr) {
      return false;
    }
    const MaterialPaintLayer *from = paste_source_row(*ctx.bmain, source);
    MaterialPaintLayer *target = paint_description_row_for_ordinal(
        const_cast<Material &>(layers_owner(owner)), target_ordinal);
    return from != nullptr && target != nullptr &&
           BKE_paint_layers_correction_can_paste(*from, *target);
  }

  bool rows_paste_into(bContext &C,
                       const StackFocus & /*focus*/,
                       ID &owner,
                       const Span<StackItemIdentity> sources,
                       const int target_ordinal,
                       Vector<StackItemIdentity> &r_created,
                       ReportList *reports) const override
  {
    Main *bmain = CTX_data_main(&C);
    if (bmain == nullptr) {
      return false;
    }
    Material &material = layers_owner(owner);
    MaterialPaintLayer *target = paint_description_row_for_ordinal(material, target_ordinal);
    if (target == nullptr) {
      return false;
    }

    /* Why two passes: a base mask must land first, or the items copied after it would find no base
     * to sit over and be refused. */
    int skipped = 0;
    bool any = false;
    for (const bool base_pass : {true, false}) {
      for (const StackItemIdentity &identity : sources) {
        const MaterialPaintLayer *from = paste_source_row(*bmain, identity);
        if (from == nullptr) {
          if (base_pass) {
            skipped++;
          }
          continue;
        }
        const bool is_base = (from->flag & MA_PAINT_LAYER_MASK_BASE) != 0;
        if (is_base != base_pass) {
          continue;
        }
        MaterialPaintLayer *copy = BKE_paint_layers_correction_paste(*bmain, material, *from, target);
        if (copy == nullptr) {
          skipped++;
          continue;
        }
        StackItemIdentity created;
        created.owner_uid = owner.session_uid;
        created.source_type = SO_STACK_SRC_PAINT_MATERIAL;
        created.row_id = copy->marker;
        created.ordinal_hint = int16_t(layers_ordinal_of(material, copy));
        r_created.append(created);
        any = true;
      }
    }
    if (skipped > 0) {
      BKE_reportf(reports, RPT_INFO, "Skipped %d item(s) that cannot be pasted here", skipped);
    }
    if (any) {
      WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    }
    return any;
  }

  /* ---------------------------------------------------------------- */
  /** \name StackGroupingEditor
   * \{ */

  bool row_mask_set(bContext &C,
                    const StackFocus & /*focus*/,
                    ID &owner,
                    const int ordinal,
                    const bool add,
                    const float initial_color[4]) const override
  {
    Material &material = layers_owner(owner);
    if (!add) {
      if (MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal)) {
        sculpt_paint::material_layer::mask_edit_end_if_target_removed(
            C, material, layer->marker);
      }
    }
    else if (MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal)) {
      if (BKE_paint_layers_mask_base(*layer) != nullptr) {
        BKE_report(CTX_wm_reports(&C), RPT_WARNING, "This layer already has a mask");
        return false;
      }
    }
    if (!paint_layers_edit_mask_set(material, ordinal, add)) {
      return false;
    }
    if (add && initial_color != nullptr) {
      /* The mask item gets its map now, in the colour the user picked (white shows the row, black
       * hides it until painted in), rather than on the first stroke. */
      MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
      Main *bmain = CTX_data_main(&C);
      Scene *scene = CTX_data_scene(&C);
      MaterialPaintLayer *item = (layer != nullptr) ? BKE_paint_layers_mask_base(*layer) : nullptr;
      if (layer != nullptr && item != nullptr && bmain != nullptr) {
        PaintLayersTarget target;
        target.material = &material;
        target.layer = layer;
        target.mask_item = item;
        target.mode = PaintLayersTargetMode::Mask;
        const int size = (scene != nullptr && scene->toolsettings != nullptr &&
                          scene->toolsettings->paint_mode.new_channel_image_size > 0) ?
                             scene->toolsettings->paint_mode.new_channel_image_size :
                             1024;
        BKE_paint_layers_target_ensure_writable(*bmain, target, size, initial_color);
      }
    }
    if (add) {
      /* A mask the user just added is the one they mean to work on: make its layer active and point
       * the brush at the mask, as adding a mask correction does. */
      if (MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal)) {
        BKE_paint_layers_active_set(material, layer->marker);
      }
      layers_target_mode_set(C, PAINT_LAYER_TARGET_MASK);
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool row_mask_toggle(bContext &C,
                       const StackFocus & /*focus*/,
                       ID &owner,
                       const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
    if (layer == nullptr) {
      return false;
    }
    const Vector<MaterialPaintLayer *> items = BKE_paint_layers_mask_items(*layer);
    if (items.is_empty()) {
      return false;
    }
    /* Turning the mask off takes the coverage the strokes were shaping away: leave mask editing
     * first, the same as a remove. */
    MaterialPaintLayer *base = BKE_paint_layers_mask_base(*layer);
    if (base != nullptr && (base->flag & MA_PAINT_LAYER_ENABLED) != 0) {
      sculpt_paint::material_layer::mask_edit_end_if_target_removed(C, material, layer->marker);
    }
    if (!paint_layers_edit_mask_toggle(material, ordinal)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  int row_merge_down(bContext &C,
                     const StackFocus & /*focus*/,
                     ID &owner,
                     const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    const int folder = paint_layers_edit_merge_down(material, ordinal);
    if (folder < 0) {
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return folder;
  }

  int rows_group(bContext &C,
                 const StackFocus & /*focus*/,
                 ID &owner,
                 const int from_ordinal,
                 const int to_ordinal) const override
  {
    Material &material = layers_owner(owner);
    const int folder = paint_layers_edit_group_range(material, from_ordinal, to_ordinal);
    if (folder < 0) {
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return folder;
  }

  int row_ungroup(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    const int child_num = paint_layers_edit_ungroup(material, ordinal);
    if (child_num < 0) {
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return child_num;
  }

  int group_add(bContext &C,
                const StackFocus & /*focus*/,
                ID &owner,
                const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    const int folder = paint_layers_edit_group_add(material, ordinal);
    if (folder < 0) {
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return folder;
  }

  bool row_color_tag_set(bContext &C,
                         const StackFocus & /*focus*/,
                         ID &owner,
                         const int ordinal,
                         const int color_tag) const override
  {
    Material &material = layers_owner(owner);
    if (!paint_layers_edit_color_tag(material, ordinal, color_tag)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  /* ---------------------------------------------------------------- */
  /** \name StackColorEditor
   *
   * A layered fill is a constant, so there is no pixel session to keep: every call is one-shot.
   * \{ */

  StackColorSession *color_session_new() const override
  {
    return nullptr;
  }

  void color_session_restore(StackColorSession & /*session*/) const override {}

  void color_session_free(StackColorSession * /*session*/) const override {}

  void color_session_push_undo(StackColorSession & /*session*/,
                               const StringRefNull /*undo_name*/) const override
  {
  }

  bool row_fill_color_set(bContext &C,
                          const StackFocus & /*focus*/,
                          ID &owner,
                          const int ordinal,
                          const float color[4],
                          StackColorSession * /*session*/) const override
  {
    Material &material = layers_owner(owner);
    if (!paint_layers_edit_fill_color(material, ordinal, color)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool row_fill_color_preview(bContext &C,
                              const StackFocus & /*focus*/,
                              ID &owner,
                              const int ordinal,
                              const float color[4],
                              StackColorSession * /*session*/) const override
  {
    Material &material = layers_owner(owner);
    if (!paint_layers_edit_fill_color(material, ordinal, color)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  /* ---------------------------------------------------------------- */
  /** \name StackDropHandler
   *
   * Three drops, as in the graph-truth source this replaced: a row of this stack is moved; an image
   * dropped onto a Fill row becomes that row's map for the channel the user picks, and beside a
   * row (or over empty space) becomes a new Fill layer of its own reading it; a material is baked
   * into a new Material layer. Images and materials may come from the file, an asset library or,
   * for images, a file dropped from outside Blender (#drop_resolve).
   * \{ */

  bool can_accept(const StackReadContext &ctx,
                  const ID &owner,
                  const StackDropPayload &payload,
                  const StackDropTarget &target,
                  const char **r_disabled_hint) const override
  {
    if (payload.id_type == ID_IM && payload.id_uid != 0) {
      if (!this->can_reorder(owner)) {
        *r_disabled_hint = TIP_("The layered material is linked or overridden");
        return false;
      }
      if (BLI_uuid_is_nil(target.anchor.row_id) || target.place != StackMovePlace::Into) {
        /* A new layer takes the image: on top, or beside the row the drop was aimed at. */
        return true;
      }
      const Material &material = layers_owner(owner);
      const MaterialPaintLayer *row = BKE_paint_layers_find(const_cast<Material &>(material),
                                                            target.anchor.row_id);
      if (row == nullptr) {
        return false;
      }
      /* A Fill row is never painted, so it takes the user's own maps: a stack Fill layer
       * as well as a Fill correction. */
      if (ELEM(BKE_paint_layers_role(*row), PaintLayerRole::Layer, PaintLayerRole::Effect) &&
          row->source == MA_PAINT_LAYER_SOURCE_CONSTANT)
      {
        return true;
      }
      /* A Paint row owns the maps its brush writes into, so a drop must not swap them; a folder
       * composites its children and a Material layer is its source's bake. */
      *r_disabled_hint = (BKE_paint_layers_role(*row) == PaintLayerRole::Layer &&
                          row->source == MA_PAINT_LAYER_SOURCE_IMAGE) ?
                             TIP_("The maps of a Paint layer cannot be replaced; drop the image "
                                  "between rows to add a Fill layer") :
                             TIP_("Only a Fill row takes an image; drop it between rows to add "
                                  "a Fill layer");
      return false;
    }
    /* A dropped material becomes a Material layer baked from it; the same gesture the tab's "New
     * Material Layer" uses. */
    if (payload.id_type == ID_MA && payload.id_uid != 0) {
      if (!this->can_reorder(owner)) {
        *r_disabled_hint = TIP_("The layered material is linked or overridden");
        return false;
      }
      /* The same refusals the Add verb reports: the drop states them up front, as a hint on the
       * gesture the user is making right now. */
      const Material &material = layers_owner(owner);
      for (Material &candidate : ctx.bmain->materials) {
        if (candidate.id.session_uid == payload.id_uid) {
          if (&candidate == &material) {
            *r_disabled_hint = TIP_("A layered material cannot bake itself");
            return false;
          }
          if (BKE_paint_layers_material_depends_on(candidate, material)) {
            *r_disabled_hint = TIP_(
                "That material already bakes this one; it would form a cycle");
            return false;
          }
          break;
        }
      }
      return true;
    }
    if (!payload.source_item.is_valid() ||
        payload.source_item.source_type != SO_STACK_SRC_PAINT_MATERIAL)
    {
      return false;
    }
    if (payload.source_item.owner_uid != owner.session_uid ||
        target.anchor.owner_uid != owner.session_uid)
    {
      return false;
    }
    if (!this->can_reorder(owner)) {
      *r_disabled_hint = TIP_("The layered material is linked or overridden");
      return false;
    }
    const Material &material = layers_owner(owner);
    return BKE_paint_layers_find(const_cast<Material &>(material), payload.source_item.row_id) !=
           nullptr;
  }

  bool execute(bContext &C,
               const StackFocus &focus,
               ID &owner,
               const StackDropPayload &payload,
               const StackDropTarget &target,
               const wmEvent *event,
               int *r_affected_ordinal) const override
  {
    const char *hint = nullptr;
    if (!this->can_accept(outliner_stack_read_context(C), owner, payload, target, &hint)) {
      return false;
    }
    Material &material = layers_owner(owner);
    MaterialPaintLayer *anchor = BLI_uuid_is_nil(target.anchor.row_id) ?
                                     nullptr :
                                     BKE_paint_layers_find(material, target.anchor.row_id);

    if (payload.id_type == ID_IM && payload.id_uid != 0) {
      /* The channel is the user's choice, made in the popup the assign operator opens; the
       * properties carry the image and where it goes. The popup's own call is the undo step. */
      PointerRNA props = WM_operator_properties_create(
          "OUTLINER_OT_stack_layer_channel_image_assign");
      RNA_int_set(&props, "image_uid", int(payload.id_uid));
      char marker_str[UUID_STRING_SIZE];
      if (anchor != nullptr) {
        BLI_uuid_format(marker_str, anchor->marker);
        RNA_string_set(&props,
                       (target.place == StackMovePlace::Into) ? "layer" : "anchor",
                       marker_str);
      }
      RNA_boolean_set(&props, "below", target.place == StackMovePlace::Below);
      const wmOperatorStatus status = WM_operator_name_call(
          &C,
          "OUTLINER_OT_stack_layer_channel_image_assign",
          wm::OpCallContext::InvokeDefault,
          &props,
          event);
      WM_operator_properties_free(&props);
      return (status & (OPERATOR_FINISHED | OPERATOR_INTERFACE)) != 0;
    }

    if (payload.id_type == ID_MA && payload.id_uid != 0) {
      Main *bmain = outliner_stack_read_context(C).bmain;
      Material *source = nullptr;
      if (bmain != nullptr) {
        for (Material &candidate : bmain->materials) {
          if (candidate.id.session_uid == payload.id_uid) {
            source = &candidate;
            break;
          }
        }
      }
      if (source == nullptr) {
        return false;
      }
      MaterialPaintLayer *created =
          ed::sculpt_paint::material_layer::add_material_layer_from_material(
              C, material, *source, anchor, PaintLayerPlace(int(target.place)));
      if (created == nullptr) {
        return false;
      }
      BKE_paint_layers_active_set(material, created->marker);
      WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
      WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
      if (r_affected_ordinal != nullptr) {
        *r_affected_ordinal = layers_ordinal_of(material, created);
      }
      /* #OUTLINER_OT_stack_layer_id_drop carries no #OPTYPE_UNDO (an image drop only opens a popup
       * whose own call is the step), so the material drop pushes its own. */
      ED_undo_push(&C, "Add Material Layer");
      return true;
    }

    MaterialPaintLayer *from = BKE_paint_layers_find(material, payload.source_item.row_id);
    if (from == nullptr) {
      return false;
    }
    const int from_ordinal = layers_ordinal_of(material, from);
    const int anchor_ordinal = (anchor != nullptr) ? layers_ordinal_of(material, anchor) : -1;
    if (from_ordinal < 0 || (anchor != nullptr && anchor_ordinal < 0)) {
      return false;
    }
    return this->row_move(C, focus, owner, from_ordinal, anchor_ordinal, target.place,
                          r_affected_ordinal);
  }

  void drop_id_types(Vector<short> &r_types) const override
  {
    r_types.append(ID_IM);
    r_types.append(ID_MA);
  }

  /** Only an image can come from a bare file: a material always lives in a .blend library. */
  bool drop_external_poll(const wmDrag &drag) const override
  {
    if (drag.type != WM_DRAG_PATH) {
      return false;
    }
    const char *path = WM_drag_get_single_path(&drag);
    return path != nullptr && BLI_path_extension_check_array(path, imb_ext_image) &&
           BLI_exists(path);
  }

  /** An image dropped onto a Paint row is assigned to it; a material is always a row of its own. */
  bool drop_supports_into(short id_type) const override
  {
    return id_type == ID_IM;
  }

  /**
   * Resolve \a drag into a local image or material: a local data-block, an asset (a bare
   * file-backed image asset included) or a dropped image file. A material asset is always
   * imported with "Append & Reuse": the layer re-bakes from it, so it has to be local, and one
   * shared copy is better than a new one per drop.
   */
  ID *drop_resolve(bContext &C, wmDrag &drag) const override
  {
    Main &bmain = *CTX_data_main(&C);

    if (drag.type == WM_DRAG_PATH) {
      if (!this->drop_external_poll(drag)) {
        return nullptr;
      }
      Image *image = BKE_image_load_exists(&bmain, WM_drag_get_single_path(&drag), nullptr);
      if (image != nullptr) {
        id_us_min(&image->id);
      }
      return (image != nullptr) ? &image->id : nullptr;
    }

    if (drag.type == WM_DRAG_ASSET_LIST) {
      const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
      if (asset_drags == nullptr) {
        return nullptr;
      }
      for (const wmDragAssetListItem &item : *asset_drags) {
        const ID_Type item_idtype = item.is_external ?
                                        item.asset_data.external_info->asset->get_id_type() :
                                        (item.asset_data.local_id ?
                                             GS(item.asset_data.local_id->name) :
                                             ID_Type(0));
        if (item_idtype == ID_IM) {
          if (!item.is_external) {
            return item.asset_data.local_id;
          }
          Image *image = ed::asset::resolve_image_from_asset(
              bmain, *item.asset_data.external_info->asset);
          if (image != nullptr && image->id.asset_data == nullptr) {
            ed::asset::image_mark_as_asset(image);
          }
          return (image != nullptr) ? &image->id : nullptr;
        }
        if (item_idtype == ID_MA) {
          if (!item.is_external) {
            return item.asset_data.local_id;
          }
          return ed::asset::asset_local_id_ensure_imported(bmain,
                                                            *item.asset_data.external_info->asset,
                                                            0,
                                                            ASSET_IMPORT_APPEND_REUSE,
                                                            std::nullopt,
                                                            CTX_wm_reports(&C));
        }
      }
      return nullptr;
    }

    /* A plain local data-block or a single asset drag. */
    if (ID *local = WM_drag_get_local_ID_or_import_from_asset(&C, &drag, ID_IM)) {
      return local;
    }
    if (drag.type == WM_DRAG_ASSET) {
      if (wmDragAsset *asset_drag = WM_drag_get_asset_data(&drag, ID_IM)) {
        Image *image = ed::asset::resolve_image_from_asset(bmain, *asset_drag->asset);
        if (image != nullptr && image->id.asset_data == nullptr) {
          ed::asset::image_mark_as_asset(image);
        }
        return (image != nullptr) ? &image->id : nullptr;
      }
    }
    if (ID *local = WM_drag_get_local_ID(&drag, ID_MA)) {
      return local;
    }
    if (drag.type == WM_DRAG_ASSET) {
      if (wmDragAsset *asset_drag = WM_drag_get_asset_data(&drag, ID_MA)) {
        return ed::asset::asset_local_id_ensure_imported(bmain,
                                                         *asset_drag->asset,
                                                         0,
                                                         ASSET_IMPORT_APPEND_REUSE,
                                                         std::nullopt,
                                                         CTX_wm_reports(&C));
      }
    }
    return nullptr;
  }

  std::string drop_tooltip(short id_type,
                           StringRef item_name,
                           StringRef row_name,
                           const StackDropTarget &target) const override
  {
    if (id_type == ID_MA) {
      if (row_name.is_empty()) {
        return fmt::format(fmt::runtime(TIP_("Add {} as a Material layer on top")), item_name);
      }
      return (target.place == StackMovePlace::Below) ?
                 fmt::format(fmt::runtime(TIP_("Add {} as a Material layer below {}")),
                             item_name,
                             row_name) :
                 fmt::format(fmt::runtime(TIP_("Add {} as a Material layer above {}")),
                             item_name,
                             row_name);
    }
    /* An image, or a drag that only resolves at drop time: a bare file can only become an image. */
    if (row_name.is_empty()) {
      return fmt::format(fmt::runtime(TIP_("Add {} as a new Fill layer on top")), item_name);
    }
    switch (target.place) {
      case StackMovePlace::Into:
        return fmt::format(fmt::runtime(TIP_("Assign {} to {}")), item_name, row_name);
      case StackMovePlace::Below:
        return fmt::format(
            fmt::runtime(TIP_("Add {} as a new Fill layer below {}")), item_name, row_name);
      case StackMovePlace::Above:
        break;
    }
    return fmt::format(fmt::runtime(TIP_("Add {} as a new Fill layer above {}")), item_name, row_name);
  }
};

void paint_stack_rows_from_description(const Material &material,
                                       const int channel,
                                       Vector<StackRow> &r_rows)
{
  paint_stack_rows_from_description_impl(material, channel, r_rows);
}

MaterialPaintLayer *paint_description_row_for_ordinal(Material &material, const int ordinal)
{
  /* The inverse of #layers_ordinal_of: both count the same #BKE_paint_layers_foreach order. The
   * base mask item consumes an ordinal in the builder without a row of its own, and this hands
   * the item back for that ordinal -- the same answer both lookups agree on. */
  int index = 0;
  MaterialPaintLayer *found = nullptr;
  BKE_paint_layers_foreach(material,
                           [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/)
                               -> bool {
                             if (index == ordinal) {
                               found = &layer;
                               return false;
                             }
                             index++;
                             return true;
                           });
  return found;
}

/* -------------------------------------------------------------------- */
/** \name Assign Dropped Image to a Channel
 *
 * The popup an image drop opens: whichever channel the user picks is where the image lands -- on
 * the Paint layer it was dropped on, or on a new Paint layer beside the row it was aimed at (on top
 * when it named none).
 * \{ */

static const EnumPropertyItem *stack_channel_image_assign_channel_itemf(bContext * /*C*/,
                                                                        PointerRNA * /*ptr*/,
                                                                        PropertyRNA * /*prop*/,
                                                                        bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  /* The identifiers are the channel enum's own, so a script names a channel the same way here as
   * everywhere else; only the icon differs from that enum. */
  for (const EnumPropertyItem *channel_item = rna_enum_material_paint_channel_items;
       channel_item->identifier != nullptr;
       channel_item++)
  {
    if (channel_item->identifier[0] == '\0' ||
        channel_item->value == PAINT_MATERIAL_CHANNEL_CUSTOM)
    {
      continue;
    }
    EnumPropertyItem item = *channel_item;
    item.icon = paint_channel_icon(channel_item->value);
    RNA_enum_item_add(&items, &items_num, &item);
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

static wmOperatorStatus stack_channel_image_assign_invoke(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent * /*event*/)
{
  ui::PopupMenu *pup = ui::popup_menu_begin(C, IFACE_("Assign to Channel"), ICON_NONE);
  ui::Layout &layout = *ui::popup_menu_layout(pup);
  /* Each item calls this operator again with one channel chosen, in exec context -- re-invoking
   * would open this popup again. The extra properties carry the image and the target along. */
  layout.operator_context_set(wm::OpCallContext::ExecDefault);
  PointerRNA extra = layout.op_menu_enum(C, op->type, "channel", std::nullopt, ICON_NONE);
  RNA_int_set(&extra, "image_uid", RNA_int_get(op->ptr, "image_uid"));
  char marker_str[UUID_STRING_SIZE];
  RNA_string_get(op->ptr, "layer", marker_str);
  RNA_string_set(&extra, "layer", marker_str);
  RNA_string_get(op->ptr, "anchor", marker_str);
  RNA_string_set(&extra, "anchor", marker_str);
  RNA_boolean_set(&extra, "below", RNA_boolean_get(op->ptr, "below"));
  ui::popup_menu_end(C, pup);
  return OPERATOR_INTERFACE;
}

/** The layer a marker property names, or null when it is empty or names nothing. */
static MaterialPaintLayer *stack_channel_image_assign_marker_layer(Material &material,
                                                                   wmOperator &op,
                                                                   const char *prop_name)
{
  char marker_str[UUID_STRING_SIZE];
  RNA_string_get(op.ptr, prop_name, marker_str);
  bUUID marker;
  if (marker_str[0] == '\0' || !BLI_uuid_parse_string(&marker, marker_str)) {
    return nullptr;
  }
  return BKE_paint_layers_find(material, marker);
}

static wmOperatorStatus stack_channel_image_assign_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);
  ID *owner = outliner_stack_owner_get(outliner_stack_read_context(*C), *space_outliner);
  if (owner == nullptr || GS(owner->name) != ID_MA) {
    return OPERATOR_CANCELLED;
  }
  Material &material = id_cast<Material &>(*owner);
  if (!ID_IS_EDITABLE(&material.id) || ID_IS_OVERRIDE_LIBRARY(&material.id)) {
    BKE_report(op->reports, RPT_ERROR, "The layered material is linked or overridden");
    return OPERATOR_CANCELLED;
  }

  Image *image = id_cast<Image *>(
      BKE_libblock_find_session_uid(bmain, ID_IM, RNA_int_get(op->ptr, "image_uid")));
  if (image == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "The dropped image is gone");
    return OPERATOR_CANCELLED;
  }
  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  /* An image cannot be assigned to a channel outside the material's global set: the record would
   * take part nowhere. */
  if (!BKE_paint_layers_channel_in_set(material, channel)) {
    BKE_report(op->reports, RPT_ERROR, "The channel is not part of the material's channel set");
    return OPERATOR_CANCELLED;
  }

  MaterialPaintLayer *layer = nullptr;
  bool created = false;
  if (RNA_string_length(op->ptr, "layer") > 0) {
    layer = stack_channel_image_assign_marker_layer(material, *op, "layer");
    if (layer == nullptr) {
      BKE_report(op->reports, RPT_ERROR, "The layer the image was dropped on is gone");
      return OPERATOR_CANCELLED;
    }
    /* Mirrors the drop gate: only a Fill row takes a map; a Paint layer's maps belong to
     * it. In a Fill layer the maps are only assigned, never painted. */
    if (!ELEM(BKE_paint_layers_role(*layer), PaintLayerRole::Layer, PaintLayerRole::Effect) ||
        layer->source != MA_PAINT_LAYER_SOURCE_CONSTANT)
    {
      BKE_report(op->reports, RPT_ERROR, "Only a Fill row takes an image");
      return OPERATOR_CANCELLED;
    }
  }
  else {
    /* A Fill layer of its own, named after the image without the file extension it may carry. The
     * channel record below carries the image, so the Fill reads it instead of its constant. */
    char name[MAX_NAME];
    STRNCPY(name, image->id.name + 2);
    BLI_path_extension_strip(name);
    MaterialPaintLayer *anchor = stack_channel_image_assign_marker_layer(material, *op, "anchor");
    const PaintLayerPlace place = (anchor != nullptr && RNA_boolean_get(op->ptr, "below")) ?
                                      PaintLayerPlace::Below :
                                      PaintLayerPlace::Above;
    layer = BKE_paint_layers_add(material, MA_PAINT_LAYER_SOURCE_CONSTANT, name, anchor, place);
    if (layer == nullptr) {
      BKE_report(op->reports, RPT_ERROR, "Could not add a layer for the image");
      return OPERATOR_CANCELLED;
    }
    created = true;
  }

  if (BKE_paint_layers_channel_add(material, layer, channel) == nullptr ||
      !BKE_paint_layers_channel_set_image(material, layer, channel, image))
  {
    BKE_report(op->reports, RPT_ERROR, "The image could not be assigned to the channel");
    if (created) {
      /* A refused assignment must not read as a silent add. */
      BKE_paint_layers_remove(material, layer);
    }
    return OPERATOR_CANCELLED;
  }
  BKE_paint_layers_active_set(material, layer->marker);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material.id);
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_channel_image_assign(wmOperatorType *ot)
{
  ot->name = "Assign Image to Channel";
  ot->description = "Assign the dropped image to a channel of the layer, or add a new Fill layer "
                    "for it";
  ot->idname = "OUTLINER_OT_stack_layer_channel_image_assign";

  ot->invoke = stack_channel_image_assign_invoke;
  ot->exec = stack_channel_image_assign_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "channel", rna_enum_dummy_NULL_items, 0, "Channel", "Channel the image paints");
  RNA_def_enum_funcs(prop, stack_channel_image_assign_channel_itemf);
  RNA_def_int(ot->srna, "image_uid", 0, 0, INT32_MAX, "Image", "", 0, INT32_MAX);
  RNA_def_string(ot->srna,
                 "layer",
                 nullptr,
                 UUID_STRING_SIZE,
                 "Layer",
                 "Marker of the Paint layer the image was dropped on; empty adds a Fill layer "
                 "of its own");
  RNA_def_string(ot->srna,
                 "anchor",
                 nullptr,
                 UUID_STRING_SIZE,
                 "Anchor",
                 "Marker of the row a new layer is placed beside; empty puts it on top");
  RNA_def_boolean(ot->srna, "below", false, "Below", "Place a new layer below the anchor row");
}

/** \} */

}  // namespace blender::ed::outliner

std::unique_ptr<blender::ed::outliner::StackSource>
blender::ed::outliner::stack_source_paint_layers_create()
{
  return std::make_unique<PaintLayersStackSource>();
}
