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

#include <climits>
#include <functional>
#include <optional>

#include <fmt/format.h>

#include "AS_asset_representation.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
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
 * The list whose direct members include \a target -- the top-level list, a folder's children or a
 * layer's corrections -- or null when \a target is not part of \a list.
 */
ListBase *layers_owner_list(ListBase *list, const MaterialPaintLayer *target)
{
  for (MaterialPaintLayer &layer : *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(list)) {
    if (&layer == target) {
      return list;
    }
    if (ListBase *found = layers_owner_list(&layer.children, target)) {
      return found;
    }
    if (ListBase *found = layers_owner_list(&layer.effects, target)) {
      return found;
    }
    if (ListBase *found = layers_owner_list(&layer.mask_stack, target)) {
      return found;
    }
  }
  return nullptr;
}

/** The layer whose children or corrections directly hold \a target, or null at the top level. */
MaterialPaintLayer *layers_parent_layer(ListBase &list, const MaterialPaintLayer *target)
{
  for (MaterialPaintLayer &layer : *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&list)) {
    for (const MaterialPaintLayer &child :
         *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&layer.children))
    {
      if (&child == target) {
        return &layer;
      }
    }
    for (const MaterialPaintLayer &effect :
         *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&layer.effects))
    {
      if (&effect == target) {
        return &layer;
      }
    }
    for (const MaterialPaintLayer &mask_item :
         *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&layer.mask_stack))
    {
      if (&mask_item == target) {
        return &layer;
      }
    }
    if (MaterialPaintLayer *found = layers_parent_layer(layer.children, target)) {
      return found;
    }
    if (MaterialPaintLayer *found = layers_parent_layer(layer.effects, target)) {
      return found;
    }
    if (MaterialPaintLayer *found = layers_parent_layer(layer.mask_stack, target)) {
      return found;
    }
  }
  return nullptr;
}

/**
 * The ordinal #paint_stack_rows_from_description would give \a target, or -1. Walks in the same
 * order the rows are built: a row, then its content corrections, its mask corrections, then its
 * children.
 */
int layers_ordinal_of(const ListBase &list, const MaterialPaintLayer *target, int &r_index)
{
  for (const MaterialPaintLayer &layer :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
  {
    if (&layer == target) {
      return r_index;
    }
    r_index++;
    for (const MaterialPaintLayer *correction : BKE_paint_layers_effects(layer)) {
      if (correction == target) {
        return r_index;
      }
      r_index++;
    }
    for (const MaterialPaintLayer *correction : BKE_paint_layers_mask_items(layer)) {
      if (correction == target) {
        return r_index;
      }
      r_index++;
    }
    if (BKE_paint_layers_is_folder(layer)) {
      const int found = layers_ordinal_of(layer.children, target, r_index);
      if (found >= 0) {
        return found;
      }
    }
  }
  return -1;
}

int layers_ordinal_of(const Material &ma, const MaterialPaintLayer *target)
{
  if (target == nullptr) {
    return -1;
  }
  int index = 0;
  return layers_ordinal_of(ma.paint_layers, target, index);
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
 * The MASK section of \a layer: one sub-row per mask-stack item that carries a map.
 *
 * The section exists as soon as the layer has any mask item, even a constant one with no map:
 * its presence is what lets a click switch the brush to the mask target. Mask items are scalar
 * and one map for every channel, so a map lives in the item's Base-Color channel record.
 */
StackContentSection paint_mask_section_build(const Material & /*ma*/,
                                             const MaterialPaintLayer &layer)
{
  StackContentSection section;
  section.identifier = "MASK";
  section.name = IFACE_("Mask Content");
  for (const MaterialPaintLayer *item : BKE_paint_layers_mask_items(layer)) {
    Image *image = nullptr;
    for (int i = 0; i < item->channels_num; i++) {
      if (item->channels[i].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
        image = item->channels[i].image;
        break;
      }
    }
    if (image == nullptr) {
      continue;
    }
    const bool enabled = (item->flag & MA_PAINT_LAYER_ENABLED) != 0;
    StackSubRow sub;
    sub.role = PAINT_LAYER_MAP_MASK;
    sub.name = image->id.name + 2;
    sub.id = &image->id;
    sub.icon = paint_mask_state_icon(enabled);
    sub.inactive = !enabled;
    section.sub_rows.append(std::move(sub));
  }
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
      return;
    }
    /* The Normal channel forces its own combine, so it has no blend to set: only its opacity is a
     * per-pair setting. */
    const bool has_blend = channel != PAINT_MATERIAL_CHANNEL_NORMAL;
    const MaterialPaintLayerChannelSettings &settings = layer.channel_settings[channel];

    PointerRNA settings_ptr = RNA_pointer_create_discrete(
        &const_cast<Material &>(material).id,
        RNA_struct_find("MaterialPaintLayerChannelSettings"),
        const_cast<MaterialPaintLayerChannelSettings *>(&settings));
    row.value_ptr = settings_ptr;
    row.value_prop = "opacity";
    /* The flags only dim the columns; they no longer change how the control is built. */
    row.value_inherited = settings.opacity == 1.0f;
    if (has_blend) {
      row.mode_ptr = settings_ptr;
      row.mode_prop = "blend_type";
      row.mode_inherited = settings.blend < 0;
    }
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

  /* Corrections of \a layer, split by their role so an effect lists under CHANNELS and a mask item
   * under MASK, in storage order. */
  auto append_corrections = [&](const MaterialPaintLayer &layer,
                                const int parent_ordinal,
                                const int parent_depth,
                                const int8_t section) {
    const Vector<const MaterialPaintLayer *> corrections =
        (section == MA_PAINT_LAYER_SECTION_MASK) ? BKE_paint_layers_mask_items(layer) :
                                                   BKE_paint_layers_effects(layer);
    for (const MaterialPaintLayer *correction_ptr : corrections) {
      const MaterialPaintLayer &correction = *correction_ptr;
      if (ordinal > STACK_ROW_ORDINAL_MAX) {
        overflow = true;
        return;
      }
      StackRow row;
      row.ordinal = int16_t(ordinal++);
      row.depth = parent_depth + 1;
      row.parent_ordinal = int16_t(parent_ordinal);
      row.parent_section_id = (section == MA_PAINT_LAYER_SECTION_MASK) ? "MASK" : "CHANNELS";
      row.stable_id = correction.marker;
      row.enabled = (correction.flag & MA_PAINT_LAYER_ENABLED) != 0;
      row.supported = true;
      row.name = correction.name[0] != '\0' ? correction.name : "Correction";
      row.name_buffer = const_cast<char *>(correction.name);
      row.icon = ICON_MODIFIER;
      append_issue_slots(row, layer.marker, correction.marker);
      set_pair_columns(row, correction);
      r_rows.append(std::move(row));
    }
  };

  std::function<void(const ListBase &, int, int)> append_list =
      [&](const ListBase &list, const int depth, const int parent_ordinal) {
        for (const MaterialPaintLayer &layer :
             *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
        {
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
          const Vector<const MaterialPaintLayer *> mask_items = BKE_paint_layers_mask_items(layer);
          const bool has_mask = !mask_items.is_empty();
          /* The row reads as masked while any of its items is on; a switched-off first item must
           * not dim the icon when a lower one still applies. */
          bool any_mask_enabled = false;
          for (const MaterialPaintLayer *item : mask_items) {
            if ((item->flag & MA_PAINT_LAYER_ENABLED) != 0) {
              any_mask_enabled = true;
              break;
            }
          }
          row.mask_enabled = !has_mask || any_mask_enabled;
          row.supported = true;
          row.name = layer.name[0] != '\0' ? layer.name :
                     folder                      ? "Folder" :
                                                   "Layer";
          row.name_buffer = const_cast<char *>(layer.name);
          row.color_tag = layer.color_tag;
          row.icon = folder                               ? ICON_FILE_FOLDER :
                     layer.kind == MA_PAINT_LAYER_KIND_FILL ? ICON_GP_DRAW_FILL :
                     has_mask                           ? paint_mask_state_icon(row.mask_enabled) :
                                                          ICON_IMAGE_RGB;

          if (!folder) {
            StackRowPreview channels_slot;
            channels_slot.section_id = "CHANNELS";
            for (int c = 0; c < layer.channels_num; c++) {
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
            if (layer.kind == MA_PAINT_LAYER_KIND_FILL) {
              /* A Fill is its colour: the swatch is what the row lays down, and clicking it opens
               * the picker. */
              StackRowPreview fill_swatch;
              fill_swatch.is_color_swatch = true;
              copy_v4_v4(fill_swatch.color, layer.fill_color);
              fill_swatch.label = IFACE_("Fill Color");
              row.preview_slots.append(std::move(fill_swatch));
            }
            if (layer.kind == MA_PAINT_LAYER_KIND_MATERIAL) {
              /* The row's live state: one BKE answer the Source Material panel reads too, through
               * RNA. Compact: an icon with the full status in its tooltip. */
              StackRowPreview live_slot;
              PaintLayersSourceGroupRefusal live_refusal =
                  PaintLayersSourceGroupRefusal::None;
              const PaintLayerMaterialLiveStatus live_status =
                  BKE_paint_layers_material_live_status(material, layer, &live_refusal);
              switch (live_status) {
                case PaintLayerMaterialLiveStatus::Live:
                  live_slot.icon = ICON_HIDE_OFF;
                  live_slot.label = IFACE_("Live");
                  break;
                case PaintLayerMaterialLiveStatus::Baking:
                  live_slot.icon = ICON_FILE_REFRESH;
                  live_slot.label = IFACE_("Baking...");
                  break;
                case PaintLayerMaterialLiveStatus::Baked:
                  live_slot.icon = ICON_IMAGE_DATA;
                  live_slot.label = IFACE_("Baked");
                  break;
                case PaintLayerMaterialLiveStatus::Refused:
                  live_slot.icon = ICON_ERROR;
                  live_slot.label = "Refused: " +
                                    std::string(BKE_paint_layers_source_group_refusal_name(
                                        live_refusal));
                  break;
              }
              row.preview_slots.append(std::move(live_slot));
            }

            StackContentSection channels;
            channels.identifier = "CHANNELS";
            channels.name = IFACE_("Channel Textures");
            for (int c = 0; c < layer.channels_num; c++) {
              const MaterialPaintLayerChannel &record = layer.channels[c];
              if (record.image == nullptr || record.channel >= STACK_ROW_SUB_ROW_STRIDE) {
                continue;
              }
              StackSubRow sub;
              sub.role = record.channel;
              sub.name = record.image->id.name + 2;
              sub.id = &record.image->id;
              sub.icon = paint_channel_icon(record.channel);
              sub.inactive = record.state == MA_PAINT_LAYER_CHANNEL_DISABLED;
              channels.sub_rows.append(std::move(sub));
            }
            row.content_sections.append(std::move(channels));
          }

          if (has_mask) {
            row.preview_slots.append(paint_mask_slot_build(folder, row.mask_enabled));
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

          append_corrections(layer, row_ordinal, depth, MA_PAINT_LAYER_SECTION_CONTENT);
          append_corrections(layer, row_ordinal, depth, MA_PAINT_LAYER_SECTION_MASK);
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

  const bool is_correction = ELEM(kind,
                                  PAINT_STACK_ADD_CORRECTION_PAINT,
                                  PAINT_STACK_ADD_CORRECTION_FILL,
                                  PAINT_STACK_ADD_MASK_CORRECTION_PAINT,
                                  PAINT_STACK_ADD_MASK_CORRECTION_FILL);
  MaterialPaintLayer *created = nullptr;
  if (is_correction) {
    if (anchor == nullptr) {
      return -1;
    }
    MaterialPaintLayer *layer = (BKE_paint_layers_role(*anchor) != PaintLayerRole::Layer) ?
                                    layers_parent_layer(material.paint_layers, anchor) :
                                    anchor;
    /* A folder takes corrections too: content corrections edit its isolated result, mask
     * corrections edit its coverage, exactly as they do for a leaf. */
    if (layer == nullptr) {
      return -1;
    }
    const bool mask_section = ELEM(kind,
                                   PAINT_STACK_ADD_MASK_CORRECTION_PAINT,
                                   PAINT_STACK_ADD_MASK_CORRECTION_FILL);
    const bool fill_effect = ELEM(
        kind, PAINT_STACK_ADD_CORRECTION_FILL, PAINT_STACK_ADD_MASK_CORRECTION_FILL);
    created = BKE_paint_layers_correction_add(material,
                                              layer,
                                              mask_section ? MA_PAINT_LAYER_SECTION_MASK :
                                                             MA_PAINT_LAYER_SECTION_CONTENT,
                                              fill_effect ? MA_PAINT_LAYER_EFFECT_FILL :
                                                            MA_PAINT_LAYER_EFFECT_PAINT,
                                              "Correction");
  }
  else {
    PaintLayerPlace place = PaintLayerPlace::Above;
    if (anchor != nullptr && BKE_paint_layers_is_folder(*anchor)) {
      place = PaintLayerPlace::Into;
    }
    if (kind == PAINT_STACK_ADD_MATERIAL) {
      /* A Material layer bakes a source material; the source arrives in #StackAddArgs. The bake
       * itself is requested by the caller that has a context (see the Add verb), so this stays
       * description-only. */
      Material *source = (args.source != nullptr && GS(args.source->name) == ID_MA) ?
                             id_cast<Material *>(args.source) :
                             nullptr;
      if (source == nullptr) {
        return -1;
      }
      created = BKE_paint_layers_add(
          material, MA_PAINT_LAYER_KIND_MATERIAL, source->id.name + 2, anchor, place);
      if (created != nullptr && !BKE_paint_layers_set_material(material, created, source)) {
        BKE_paint_layers_remove(material, created);
        created = nullptr;
      }
      return (created != nullptr) ? layers_ordinal_of(material, created) : -1;
    }
    eMaterialPaintLayerKind layer_kind = MA_PAINT_LAYER_KIND_PAINT;
    if (kind == PAINT_STACK_ADD_FILL) {
      layer_kind = MA_PAINT_LAYER_KIND_FILL;
    }
    else if (kind == PAINT_STACK_ADD_FOLDER) {
      layer_kind = MA_PAINT_LAYER_KIND_FOLDER;
    }
    created = BKE_paint_layers_add(material, layer_kind, nullptr, anchor, place);
    if (created != nullptr) {
      /* The default channel set is a policy of its own (see the BKE helper); the Add only places
       * the row. */
      BKE_paint_layers_default_channels_apply(material, *created);
    }
    if (created != nullptr && kind == PAINT_STACK_ADD_FILL && args.color != nullptr) {
      BKE_paint_layers_set_fill_color(material, created, args.color);
    }
  }
  if (created == nullptr) {
    return -1;
  }
  return layers_ordinal_of(material, created);
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
  bool edited = false;
  /* Effect and mask-item rows are children of a layer, not stack members: they move only within
   * their owner's corrections and never into a folder. */
  auto is_correction = [](const MaterialPaintLayer *layer) {
    return layer != nullptr && BKE_paint_layers_role(*layer) != PaintLayerRole::Layer;
  };
  if (place == StackMovePlace::Into && anchor != nullptr && !BKE_paint_layers_is_folder(*anchor)) {
    if (is_correction(from) || is_correction(anchor)) {
      return false;
    }
    Vector<MaterialPaintLayer *> to_group{anchor, from};
    edited = BKE_paint_layers_group(material, to_group) != nullptr;
  }
  else if (is_correction(from) || is_correction(anchor))
  {
    if (!is_correction(from) || anchor == nullptr || !is_correction(anchor) ||
        place == StackMovePlace::Into)
    {
      return false;
    }
    ListBase *owner_list = layers_owner_list(&material.paint_layers, from);
    if (owner_list == nullptr ||
        layers_owner_list(&material.paint_layers, anchor) != owner_list)
    {
      return false;
    }
    const int anchor_index = BLI_findindex(owner_list, anchor);
    if (anchor_index < 0) {
      return false;
    }
    edited = BKE_paint_layers_reorder(
        material, from, (place == StackMovePlace::Above) ? anchor_index : anchor_index + 1);
  }
  else
  {
    PaintLayerPlace layer_place = PaintLayerPlace::Above;
    if (place == StackMovePlace::Below) {
      layer_place = PaintLayerPlace::Below;
    }
    else if (place == StackMovePlace::Into) {
      layer_place = PaintLayerPlace::Into;
    }
    edited = BKE_paint_layers_move(material, from, anchor, layer_place);
  }
  if (!edited) {
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
  /* A correction's own coverage is its mask stack; a folder takes a mask like any row. */
  if (layer == nullptr || BKE_paint_layers_role(*layer) != PaintLayerRole::Layer) {
    return false;
  }
  if (add) {
    return BKE_paint_layers_mask_add(material, layer, 1.0f) != nullptr;
  }
  const Vector<MaterialPaintLayer *> items = BKE_paint_layers_mask_items(*layer);
  return !items.is_empty() && BKE_paint_layers_remove(material, items.first());
}

bool paint_layers_edit_mask_toggle(Material &material, const int ordinal)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  if (layer == nullptr) {
    return false;
  }
  const Vector<MaterialPaintLayer *> items = BKE_paint_layers_mask_items(*layer);
  if (items.is_empty()) {
    return false;
  }
  MaterialPaintLayer *item = items.first();
  const bool enable = (item->flag & MA_PAINT_LAYER_ENABLED) == 0;
  return BKE_paint_layers_set_enabled(material, item, enable);
}

int paint_layers_edit_merge_down(Material &material, const int ordinal)
{
  MaterialPaintLayer *upper = paint_description_row_for_ordinal(material, ordinal);
  MaterialPaintLayer *lower = (ordinal > 0) ?
                                  paint_description_row_for_ordinal(material, ordinal - 1) :
                                  nullptr;
  if (upper == nullptr || lower == nullptr ||
      BKE_paint_layers_role(*upper) != PaintLayerRole::Layer ||
      BKE_paint_layers_role(*lower) != PaintLayerRole::Layer)
  {
    return -1;
  }
  ListBase *owner_list = layers_owner_list(&material.paint_layers, upper);
  if (owner_list == nullptr ||
      layers_owner_list(&material.paint_layers, lower) != owner_list)
  {
    return -1;
  }
  Vector<MaterialPaintLayer *> to_group{lower, upper};
  MaterialPaintLayer *folder = BKE_paint_layers_group(material, to_group);
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
    if (layer == nullptr || BKE_paint_layers_role(*layer) != PaintLayerRole::Layer) {
      return -1;
    }
    to_group.append(layer);
  }
  if (to_group.is_empty()) {
    return -1;
  }
  MaterialPaintLayer *folder = BKE_paint_layers_group(material, to_group);
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
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      material, MA_PAINT_LAYER_KIND_FOLDER, "Folder", anchor, PaintLayerPlace::Above);
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
  return layer != nullptr && layer->kind == MA_PAINT_LAYER_KIND_FILL &&
         BKE_paint_layers_set_fill_color(material, layer, color);
}

bool paint_layers_edit_reorder(Material &material, const int from_ordinal, const int to_ordinal)
{
  MaterialPaintLayer *from = paint_description_row_for_ordinal(material, from_ordinal);
  MaterialPaintLayer *to = paint_description_row_for_ordinal(material, to_ordinal);
  if (from == nullptr || to == nullptr || from == to ||
      BKE_paint_layers_role(*from) != PaintLayerRole::Layer ||
      BKE_paint_layers_role(*to) != PaintLayerRole::Layer)
  {
    return false;
  }
  ListBase *owner_list = layers_owner_list(&material.paint_layers, from);
  if (owner_list == nullptr ||
      layers_owner_list(&material.paint_layers, to) != owner_list)
  {
    return false;
  }
  const int index = BLI_findindex(owner_list, to);
  return index >= 0 && BKE_paint_layers_reorder(material, from, index);
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
    std::function<void(const ListBase &)> walk = [&](const ListBase &list) {
      for (const MaterialPaintLayer &layer :
           *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
      {
        hash = hash * 1000003u ^ UUID(layer.marker).hash();
        hash ^= uint64_t(layer.kind) | (uint64_t(layer.flag) << 8) |
                (uint64_t(layer.blend) << 16);
        /* The swatch and the columns show these, so a change to them has to reach the rows. */
        for (const float value : {layer.fill_color[0],
                                  layer.fill_color[1],
                                  layer.fill_color[2],
                                  layer.fill_color[3],
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
        if (layer.kind == MA_PAINT_LAYER_KIND_MATERIAL &&
            BKE_paint_layers_source_group_build_failed_get(material, layer))
        {
          /* A build failure is runtime data, not in `paint_layers_flag`: without mixing it in the
           * cached rows would keep the "Live" preview after the wrapper failed to build. */
          hash = hash * 1000003u ^ 0xB17D0FFAULL;
        }
        auto hash_corrections = [&](const ListBase &corrections) {
          for (const MaterialPaintLayer &correction :
               *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&corrections))
          {
            hash = hash * 1000003u ^ UUID(correction.marker).hash();
            hash ^= uint64_t(correction.kind) | (uint64_t(correction.flag) << 8) |
                    (uint64_t(correction.section) << 16) | (uint64_t(correction.effect) << 24);
            for (int c = 0; c < correction.channels_num; c++) {
              const Image *image = correction.channels[c].image;
              hash ^= uint64_t(correction.channels[c].channel) << 32;
              hash ^= uint64_t(correction.channels[c].state) << 40;
              if (image != nullptr) {
                hash ^= uint64_t(image->id.session_uid) * 2654435761u;
                hash = hash * 1000003u ^ uint64_t(paint_image_is_blank(*image) ? 1 : 0);
              }
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
    /* An Image Editor the user already has open is where the map appears; only when there is none
     * does the Outliner's own area become one. */
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
    ED_space_image_set(CTX_data_main(&C), space_image, id_cast<Image *>(sub_row.id), false);
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_IMAGE, space_image);
    return true;
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
                    IFACE_("Correction"),
                    "A painted adjustment hung on the content of the row this is added from",
                    ICON_BRUSH_DATA});
    r_kinds.append({"CORRECTION_FILL",
                    IFACE_("Fill Correction"),
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
      if (source == &material) {
        BKE_report(reports, RPT_ERROR, "A layered material cannot bake itself");
        return -1;
      }
      if (BKE_paint_layers_material_depends_on(*source, material)) {
        BKE_report(reports, RPT_ERROR, "That material already bakes this one; it would form a cycle");
        return -1;
      }
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

  bool can_paste_into(const StackReadContext & /*ctx*/,
                      const StackFocus & /*focus*/,
                      const ID & /*owner*/,
                      const StackItemIdentity & /*source*/,
                      const int /*target_ordinal*/) const override
  {
    /* Cross-material copy of a description row is not implemented yet. */
    return false;
  }

  bool rows_paste_into(bContext & /*C*/,
                       const StackFocus & /*focus*/,
                       ID & /*owner*/,
                       const Span<StackItemIdentity> /*sources*/,
                       const int /*target_ordinal*/,
                       Vector<StackItemIdentity> & /*r_created*/,
                       ReportList * /*reports*/) const override
  {
    return false;
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
    if (!paint_layers_edit_mask_set(material, ordinal, add)) {
      return false;
    }
    if (add && initial_color != nullptr) {
      /* The mask item gets its map now, in the colour the user picked (white shows the row, black
       * hides it until painted in), rather than on the first stroke. */
      MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
      Main *bmain = CTX_data_main(&C);
      Scene *scene = CTX_data_scene(&C);
      const Vector<MaterialPaintLayer *> items = (layer != nullptr) ?
                                                     BKE_paint_layers_mask_items(*layer) :
                                                     Vector<MaterialPaintLayer *>();
      MaterialPaintLayer *item = items.is_empty() ? nullptr : items.first();
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
    if ((items.first()->flag & MA_PAINT_LAYER_ENABLED) != 0) {
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
   * dropped onto a Paint row becomes that row's map for the channel the user picks, and beside a
   * row (or over empty space) becomes a new Paint layer of its own; a material is baked into a new
   * Material layer. Images and materials may come from the file, an asset library or, for images,
   * a file dropped from outside Blender (#drop_resolve).
   * \{ */

  bool can_accept(const StackReadContext & /*ctx*/,
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
      /* Only a Paint row is made of maps: a Fill is a colour (corrected, never painted), a folder
       * composites its children and a Material layer is its source's bake. */
      if (row->kind != MA_PAINT_LAYER_KIND_PAINT) {
        *r_disabled_hint = TIP_(
            "Only a Paint layer takes an image; drop it between rows to add a layer");
        return false;
      }
      return true;
    }
    /* A dropped material becomes a Material layer baked from it; the same gesture the tab's "New
     * Material Layer" uses. */
    if (payload.id_type == ID_MA && payload.id_uid != 0) {
      if (!this->can_reorder(owner)) {
        *r_disabled_hint = TIP_("The layered material is linked or overridden");
        return false;
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
      return fmt::format(fmt::runtime(TIP_("Add {} as a new layer on top")), item_name);
    }
    switch (target.place) {
      case StackMovePlace::Into:
        return fmt::format(fmt::runtime(TIP_("Assign {} to {}")), item_name, row_name);
      case StackMovePlace::Below:
        return fmt::format(
            fmt::runtime(TIP_("Add {} as a new layer below {}")), item_name, row_name);
      case StackMovePlace::Above:
        break;
    }
    return fmt::format(fmt::runtime(TIP_("Add {} as a new layer above {}")), item_name, row_name);
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
  int index = 0;
  MaterialPaintLayer *found = nullptr;
  std::function<void(ListBase &)> walk = [&](ListBase &list) {
    for (MaterialPaintLayer &layer : *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(&list)) {
      if (found != nullptr) {
        return;
      }
      if (index == ordinal) {
        found = &layer;
        return;
      }
      index++;
      for (MaterialPaintLayer *correction : BKE_paint_layers_effects(layer)) {
        if (found != nullptr) {
          return;
        }
        if (index == ordinal) {
          found = correction;
          return;
        }
        index++;
      }
      for (MaterialPaintLayer *correction : BKE_paint_layers_mask_items(layer)) {
        if (found != nullptr) {
          return;
        }
        if (index == ordinal) {
          found = correction;
          return;
        }
        index++;
      }
      if (BKE_paint_layers_is_folder(layer)) {
        walk(layer.children);
      }
    }
  };
  walk(material.paint_layers);
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

  MaterialPaintLayer *layer = nullptr;
  bool created = false;
  if (RNA_string_length(op->ptr, "layer") > 0) {
    layer = stack_channel_image_assign_marker_layer(material, *op, "layer");
    if (layer == nullptr) {
      BKE_report(op->reports, RPT_ERROR, "The layer the image was dropped on is gone");
      return OPERATOR_CANCELLED;
    }
    if (layer->kind != MA_PAINT_LAYER_KIND_PAINT) {
      BKE_report(op->reports, RPT_ERROR, "Only a Paint layer takes an image");
      return OPERATOR_CANCELLED;
    }
  }
  else {
    /* A layer of its own, named after the image without the file extension it may carry. */
    char name[MAX_NAME];
    STRNCPY(name, image->id.name + 2);
    BLI_path_extension_strip(name);
    MaterialPaintLayer *anchor = stack_channel_image_assign_marker_layer(material, *op, "anchor");
    const PaintLayerPlace place = (anchor != nullptr && RNA_boolean_get(op->ptr, "below")) ?
                                      PaintLayerPlace::Below :
                                      PaintLayerPlace::Above;
    layer = BKE_paint_layers_add(material, MA_PAINT_LAYER_KIND_PAINT, name, anchor, place);
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
  ot->description =
      "Assign the dropped image to a channel of the layer, or add a new layer for it";
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
                 "Marker of the Paint layer the image was dropped on; empty adds a layer of its own");
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
