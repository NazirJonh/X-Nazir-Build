/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The paint layers of a *layered* material, read from and written to the DNA description.
 *
 * `Material::paint_layers` is the truth here; the node tree is generated from it, and every verb
 * goes through the `BKE_paint_layers_*` API by marker. The source is deliberately free of the old
 * graph-as-truth vocabulary -- no bindings, no graph corrections, no node identities -- so the
 * description can be edited with or without an Outliner in the context.
 */

#include <algorithm>
#include <functional>
#include <string>
#include <type_traits>

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_uuid_types.h"

#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_report.hh"

#include "BLI_assert.h"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "RNA_access.hh"
#include "RNA_types.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "outliner_stack_source.hh"
#include "outliner_stack_source_paint_layers_intern.hh"

namespace blender::ed::outliner {

/* -------------------------------------------------------------------- */
/** \name Description helpers
 * \{ */

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
 * Rebuild the paint slots of the active object. The projection canvas paints the slots, and which
 * map each holds depends on the mode and on the active row, so a change of either has to reach them.
 */
void layers_texpaint_slots_refresh(bContext &C)
{
  Scene *scene = CTX_data_scene(&C);
  Object *object = CTX_data_active_object(&C);
  if (scene != nullptr && object != nullptr) {
    BKE_texpaint_slots_refresh_object(scene, object);
  }
}

/** Set the target a layered material's stroke paints; true when it changed. */
bool layers_target_mode_set(bContext &C, const ePaintLayerTargetMode mode)
{
  Scene *scene = CTX_data_scene(&C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return false;
  }
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  if (paint_mode.layer_target_mode == mode) {
    return false;
  }
  paint_mode.layer_target_mode = int8_t(mode);
  layers_texpaint_slots_refresh(C);
  WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, scene);
  return true;
}

/** Leave Mask mode when the layer being edited in it is the one \a layer is: its mask is going
 * away or off, and a stroke into nothing must not be what the mode means. */
void layers_mask_edit_end_if_active(bContext &C,
                                    const Material &material,
                                    const MaterialPaintLayer &layer)
{
  const Scene *scene = CTX_data_scene(&C);
  if (scene == nullptr || scene->toolsettings == nullptr ||
      scene->toolsettings->paint_mode.layer_target_mode != PAINT_LAYER_TARGET_MASK)
  {
    return;
  }
  /* The row, or a folder it holds the edited row in: removing the folder takes the mask along. */
  if (BKE_paint_layers_subtree_contains(layer, material.active_layer_marker)) {
    layers_target_mode_set(C, PAINT_LAYER_TARGET_CONTENT);
  }
}

/** The Base Color record of a mask item, which holds its constant and its map; null when absent. */
const MaterialPaintLayerChannel *paint_mask_record(const MaterialPaintLayer &item)
{
  for (int i = 0; i < item.channels_num; i++) {
    if (item.channels[i].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      return &item.channels[i];
    }
  }
  return nullptr;
}

/** A generated map whose pixels were never written is drawn as a placeholder, not as empty. */
bool paint_image_is_blank(const Image &image)
{
  if (image.source != IMA_SRC_GENERATED) {
    return false;
  }
  const ImageTile *base_tile = BKE_image_get_tile(const_cast<Image *>(&image), 0);
  return base_tile != nullptr && base_tile->gen_type == IMA_GENTYPE_BLANK &&
         base_tile->gen_color[3] == 0.0f && !BKE_image_is_dirty(const_cast<Image *>(&image));
}

/**
 * The mask slot of a row: always the Mask icon (the "clip" icon while the mask is switched off),
 * never a preview of its map. Whether the mask began white or black is unreadable once it has been
 * painted over, and a thumbnail of the map would stop saying that the slot is the mask.
 */
StackRowPreview paint_mask_slot_build(const MaterialPaintLayer &item)
{
  StackRowPreview slot;
  slot.section_id = "MASK";
  slot.label = IFACE_("Mask");
  slot.icon = ((item.flag & MA_PAINT_LAYER_ENABLED) != 0) ? ICON_MOD_MASK : ICON_CLIPUV_HLT;
  return slot;
}

}  // namespace

namespace {

/** The row of one content correction, hung off the row \a owner_ordinal names. */
StackRow paint_correction_row_build(const Material &material,
                                    const MaterialPaintLayer &effect,
                                    const int depth,
                                    const int owner_ordinal,
                                    const int ordinal)
{
  StackRow row;
  row.ordinal = int16_t(ordinal);
  row.depth = depth;
  row.parent_ordinal = int16_t(owner_ordinal);
  row.parent_section_id = "CHANNELS";
  row.stable_id = effect.marker;
  row.enabled = (effect.flag & MA_PAINT_LAYER_ENABLED) != 0;
  row.supported = true;
  row.name = effect.name[0] != '\0' ? effect.name : "Correction Paint";
  row.name_buffer = const_cast<char *>(effect.name);
  row.icon = ICON_IMAGE_RGB_ALPHA;
  /* A correction reads as a setting of its layer rather than as a member of the stack. */
  row.compact = true;

  PointerRNA effect_ptr = RNA_pointer_create_discrete(
      &const_cast<Material &>(material).id,
      RNA_struct_find("MaterialPaintLayer"),
      const_cast<MaterialPaintLayer *>(&effect));
  row.value_ptr = effect_ptr;
  row.value_prop = "opacity";
  row.value_inherited = (effect.opacity == 1.0f);
  row.mode_ptr = effect_ptr;
  row.mode_prop = "blend_type";
  row.mode_inherited = (effect.blend == MA_PAINT_LAYER_BLEND_MIX);

  /* The slot shows the correction's first map, blank or not, and switches the brush to content. */
  StackRowPreview slot;
  slot.section_id = "CHANNELS";
  for (int c = 0; c < effect.channels_num; c++) {
    const Image *image = effect.channels[c].image;
    if (image == nullptr) {
      continue;
    }
    slot.id_uid = image->id.session_uid;
    slot.id_type = ID_IM;
    slot.is_blank = paint_image_is_blank(*image);
    break;
  }
  row.preview_slots.append(std::move(slot));
  StackContentSection channels;
  channels.identifier = "CHANNELS";
  channels.name = IFACE_("Channel Textures");
  row.content_sections.append(std::move(channels));
  return row;
}

/**
 * Append the rows of \a list, and recursively of every folder in it, depth first. A folder's row
 * is appended after what it holds: the tree is built walking the rows from the last one down, and a
 * child is only hung off a folder already made. The ordinals keep the walk order -- rows are looked
 * up by ordinal, never by position -- which is the one #BKE_paint_layers_foreach defines.
 */
void paint_stack_rows_append(const Material &material,
                             const ListBaseT<MaterialPaintLayer> &list,
                             const int depth,
                             const int parent_ordinal,
                             int &ordinal,
                             bool &overflow,
                             Vector<StackRow> &r_rows)
{
  for (const MaterialPaintLayer &layer : list) {
    if (overflow) {
      return;
    }
    if (ordinal > STACK_ROW_ORDINAL_MAX) {
      overflow = true;
      return;
    }
    const bool folder = BKE_paint_layers_is_folder(layer);
    StackRow row;
    row.ordinal = int16_t(ordinal++);
    row.depth = depth;
    row.parent_ordinal = int16_t(parent_ordinal);
    row.stable_id = layer.marker;
    row.can_hold_children = folder;
    row.has_children = BKE_paint_layers_has_children(layer);
    row.enabled = (layer.flag & MA_PAINT_LAYER_ENABLED) != 0;
    row.supported = true;
    row.name = layer.name[0] != '\0' ? layer.name : (folder ? "Folder" : "Layer");
    row.name_buffer = const_cast<char *>(layer.name);
    row.icon = folder                                         ? ICON_FILE_FOLDER :
               (layer.source == MA_PAINT_LAYER_SOURCE_CONSTANT) ? ICON_GP_DRAW_FILL :
                                                                  ICON_IMAGE_RGB;

    /* The value column edits the row's opacity; a value left at its default reads as inherited
     * rather than deliberately set, so the control dims until the user moves it. */
    PointerRNA layer_ptr = RNA_pointer_create_discrete(
        &const_cast<Material &>(material).id,
        RNA_struct_find("MaterialPaintLayer"),
        const_cast<MaterialPaintLayer *>(&layer));
    row.value_ptr = layer_ptr;
    row.value_prop = "opacity";
    row.value_inherited = (layer.opacity == 1.0f);

    /* The mode column edits how the row blends over what is below it; Mix is the inherited
     * default, so the control dims until the user picks another mode. */
    row.mode_ptr = layer_ptr;
    row.mode_prop = "blend_type";
    row.mode_inherited = (layer.blend == MA_PAINT_LAYER_BLEND_MIX);

    if (folder) {
      /* A folder is what it holds: it has no map, so no channel slot. Its content section is the
       * one the brush switches back to from the mask. */
      StackContentSection content;
      content.identifier = "CHANNELS";
      content.name = IFACE_("Content");
      row.content_sections.append(std::move(content));
    }
    else if (layer.source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
      /* A Fill is its colour: one swatch with the bucket drawn over it, and no channel-texture
       * placeholder. With the Base Color record removed or switched off the row lays no colour, so
       * no swatch is drawn. */
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
      /* The content slot shows the row's first channel map, blank or not. */
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
    }

    if (!folder) {
      /* The channel maps are the layer's internals rather than rows of their own; the section
       * stays so the brush can switch to the content and later rows can attach to it. */
      StackContentSection channels;
      channels.identifier = "CHANNELS";
      channels.name = IFACE_("Channel Textures");
      row.content_sections.append(std::move(channels));
    }

    /* The mask: a slot of its own, whose click switches the brush to it, and the section that slot
     * reveals. Its switch rides on the row, so a switched-off mask is kept and listed. */
    if (const MaterialPaintLayer *mask = BKE_paint_layers_mask_base(layer)) {
      if (folder) {
        /* The slot that switches back to the content, ahead of the mask's. The folder icon stays
         * in front of it, as it does for a folder with no mask. */
        StackRowPreview content_slot;
        content_slot.section_id = "CHANNELS";
        content_slot.icon = ICON_BRUSHES_ALL;
        content_slot.keeps_row_icon = true;
        content_slot.label = IFACE_("Content");
        row.preview_slots.append(std::move(content_slot));
      }
      row.preview_slots.append(paint_mask_slot_build(*mask));
      row.aux_enabled = (mask->flag & MA_PAINT_LAYER_ENABLED) != 0;
      StackContentSection mask_section;
      mask_section.identifier = "MASK";
      mask_section.name = IFACE_("Mask Content");
      row.content_sections.append(std::move(mask_section));
    }

    const int row_ordinal = row.ordinal;
    /* The corrections are dense settings of their layer: rows attached to its content section,
     * numbered right after it (see #paint_rows_walk). */
    for (const MaterialPaintLayer &effect : layer.effects) {
      if (ordinal > STACK_ROW_ORDINAL_MAX) {
        overflow = true;
        return;
      }
      r_rows.append(paint_correction_row_build(material, effect, depth + 1, row_ordinal, ordinal++));
    }
    if (folder) {
      paint_stack_rows_append(
          material, layer.children, depth + 1, row_ordinal, ordinal, overflow, r_rows);
    }
    r_rows.append(std::move(row));
  }
}

}  // namespace

void paint_stack_rows_from_description(const Material &material,
                                       const int channel,
                                       Vector<StackRow> &r_rows)
{
  /* The value/mode columns address the selected channel; until the per (row, channel) settings
   * exist they fall back to the row's own opacity, so the channel is not read here yet. */
  static_cast<void>(channel);

  int ordinal = 0;
  bool overflow = false;
  paint_stack_rows_append(material, material.paint_layers, 0, -1, ordinal, overflow, r_rows);
  if (overflow) {
    StackRow overflow_row;
    overflow_row.ordinal = STACK_ROW_ORDINAL_MAX + 1;
    overflow_row.supported = false;
    overflow_row.unsupported_reason = "Stack is too large to display";
    overflow_row.icon = ICON_IMAGE_RGB;
    r_rows.append(std::move(overflow_row));
  }
}

namespace {

/**
 * Visit every row the Outliner lists in the order it numbers them: a row, then its corrections,
 * then what a folder holds. #paint_stack_rows_append hands out the ordinals in this same order, so
 * the two cannot disagree about which row an ordinal names. Stops when \a fn returns false.
 *
 * \a ListT is the list of rows or its const form, so a caller that needs to edit the row it finds
 * walks the mutable list instead of casting the constness away afterwards.
 */
template<typename ListT, typename Fn> bool paint_rows_walk(ListT &list, const Fn &fn)
{
  using RowT = std::remove_reference_t<decltype(*list.begin())>;
  for (RowT &layer : list) {
    if (!fn(layer)) {
      return false;
    }
    for (RowT &effect : layer.effects) {
      if (!fn(effect)) {
        return false;
      }
    }
    if (BKE_paint_layers_is_folder(layer) && !paint_rows_walk(layer.children, fn)) {
      return false;
    }
  }
  return true;
}

}  // namespace

MaterialPaintLayer *paint_description_row_for_ordinal(Material &material, const int ordinal)
{
  if (ordinal < 0) {
    return nullptr;
  }
  /* The ordinal is the row's place in the depth-first walk the row builder numbers by. */
  int index = 0;
  MaterialPaintLayer *found = nullptr;
  paint_rows_walk(material.paint_layers, [&](MaterialPaintLayer &layer) -> bool {
    if (index == ordinal) {
      found = &layer;
      return false;
    }
    index++;
    return true;
  });
  return found;
}

int paint_layers_ordinal_of(const Material &material, const MaterialPaintLayer *target)
{
  if (target == nullptr) {
    return -1;
  }
  int index = 0;
  int found = -1;
  paint_rows_walk(material.paint_layers, [&](const MaterialPaintLayer &layer) -> bool {
    if (&layer == target) {
      found = index;
      return false;
    }
    index++;
    return true;
  });
  return found;
}

bool paint_layers_edit_rename(Material &material, const int ordinal, const char *name)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  return layer != nullptr && BKE_paint_layers_rename(material, layer, name);
}

int paint_layers_edit_duplicate(Main &bmain, Material &material, const int ordinal)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  if (layer == nullptr) {
    return -1;
  }
  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(bmain, material, layer);
  return (copy != nullptr) ? paint_layers_ordinal_of(material, copy) : -1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The source
 * \{ */

class PaintLayersStackSource final : public StackSource,
                                      public StackEditor,
                                      public StackGroupingEditor {
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
    return material != nullptr && BKE_paint_layers_is_layered(*material);
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

  uint64_t state_hash(const StackReadContext &ctx, const ID &owner) const override
  {
    const Material &material = layers_owner(owner);
    uint64_t hash = uint64_t(material.paint_layers_flag);
    /* The header's channel cursor changes which pair the columns address, so the rows rebuild
     * when it moves. */
    hash = hash * 1000003u ^ uint64_t(paint_stack_selected_channel(ctx));
    BKE_paint_layers_foreach(
        material,
        [&](const MaterialPaintLayer &layer, const MaterialPaintLayer *parent) -> bool {
      /* The nesting is part of the rows: moving a row into a folder changes no marker. */
      hash = hash * 1000003u ^ (parent != nullptr ? UUID(parent->marker).hash() : 0);
      hash = hash * 1000003u ^ UUID(layer.marker).hash();
      hash ^= uint64_t(layer.flag);
      /* Mixed, not shifted into the flag bits: `flag` is 16 bits wide and would overlap. */
      hash = hash * 1000003u ^ uint64_t(layer.blend);
      hash ^= uint64_t(layer.source) << 16;
      /* The swatch and the columns show these, so a change to them has to reach the rows. */
      hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(layer.opacity));
      for (const char *ch = layer.name; *ch != '\0'; ch++) {
        hash = hash * 131u ^ uint64_t(uint8_t(*ch));
      }
      /* The mask slot shows the mask's map, or its constant before it has one. */
      if (const MaterialPaintLayer *mask = BKE_paint_layers_mask_base(layer)) {
        hash = hash * 1000003u ^ uint64_t(mask->flag);
        if (const MaterialPaintLayerChannel *record = paint_mask_record(*mask)) {
          hash = hash * 1000003u ^
                 uint64_t(std::hash<float>{}(BKE_paint_layers_mask_constant(*mask)));
          if (record->image != nullptr) {
            hash ^= uint64_t(record->image->id.session_uid) * 2654435761u;
            hash = hash * 1000003u ^ uint64_t(paint_image_is_blank(*record->image) ? 1 : 0);
          }
        }
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
      /* The corrections are rows of their own: what their rows show has to reach the tree too. */
      for (const MaterialPaintLayer &effect : layer.effects) {
        hash = hash * 1000003u ^ UUID(effect.marker).hash();
        hash ^= uint64_t(effect.flag);
        hash = hash * 1000003u ^ uint64_t(effect.blend);
        hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(effect.opacity));
        for (const char *ch = effect.name; *ch != '\0'; ch++) {
          hash = hash * 131u ^ uint64_t(uint8_t(*ch));
        }
        for (int c = 0; c < effect.channels_num; c++) {
          const Image *image = effect.channels[c].image;
          hash ^= uint64_t(effect.channels[c].channel) << 32;
          hash ^= uint64_t(effect.channels[c].state) << 40;
          if (image != nullptr) {
            hash ^= uint64_t(image->id.session_uid) * 2654435761u;
            hash = hash * 1000003u ^ uint64_t(paint_image_is_blank(*image) ? 1 : 0);
          }
        }
      }
      return true;
    });
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
    StackColumnLayout layout;
    layout.value_width = 2.6f;
    /* Wide enough for the compact "NN% Abr" label; the blend column shares the width. */
    layout.mode_width = 2.6f;
    layout.action_width = 0.0f;
    return layout;
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
    /* The mode is global: a layer with no mask is painted on its content, not left as a stroke
     * into nothing. */
    const MaterialPaintLayer *activated = BKE_paint_layers_active_layer_get(material);
    if (activated != nullptr && BKE_paint_layers_mask_base(*activated) == nullptr) {
      layers_target_mode_set(C, PAINT_LAYER_TARGET_CONTENT);
    }
    /* The mode may have stayed Mask while the row changed: the slots hold the new row's mask map,
     * and a change of mode above already refreshed them once more, which is cheap. */
    layers_texpaint_slots_refresh(C);
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
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
    /* A click on a slot switches what the brush paints: the mask, or the content. */
    if (section_id == "MASK") {
      return layers_target_mode_set(C, PAINT_LAYER_TARGET_MASK);
    }
    if (section_id == "CHANNELS") {
      return layers_target_mode_set(C, PAINT_LAYER_TARGET_CONTENT);
    }
    return false;
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

  void sub_selections_get(const StackReadContext &ctx,
                          const StackFocus &focus,
                          Vector<StackSubSelection> &r_items) const override
  {
    Object *object = outliner_stack_focus_object_get(ctx, focus);
    if (object == nullptr) {
      return;
    }
    /* One entry per material slot: the stack the Outliner shows is one material's, and an object
     * can carry several. */
    for (const int slot : IndexRange(object->totcol)) {
      const Material *material = BKE_object_material_get(object, short(slot + 1));
      StackSubSelection item;
      item.name = material != nullptr ? material->id.name + 2 : "";
      item.preview_id = (material != nullptr) ? &const_cast<Material *>(material)->id : nullptr;
      r_items.append(std::move(item));
    }
  }

  int default_sub_index(const StackReadContext &ctx, const StackFocus &focus) const override
  {
    Object *object = outliner_stack_focus_object_get(ctx, focus);
    return (object != nullptr && object->actcol > 0) ? int(object->actcol) - 1 : -1;
  }

  void sub_selection_apply(bContext & /*C*/,
                           const StackFocus &focus,
                           Object &object) const override
  {
    /* Choosing a stack slot is choosing the object's active material slot. */
    if (focus.sub_index >= 0 && focus.sub_index < object.totcol) {
      object.actcol = short(focus.sub_index + 1);
      /* As `Object.active_material_index` does: an edit-mesh assigns new faces from its own copy
       * of the slot. */
      if (object.type == OB_MESH) {
        Mesh *mesh = id_cast<Mesh *>(object.data);
        if (mesh->runtime->edit_mesh) {
          mesh->runtime->edit_mesh->mat_nr = focus.sub_index;
        }
      }
    }
  }

  const StackEditor *editor() const override
  {
    return this;
  }

  StringRefNull default_section_id() const override
  {
    return "CHANNELS";
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
    /* A first stroke materializes an image the rows list. */
    if (notifier.category == NC_IMAGE) {
      return ELEM(notifier.action, NA_ADDED, NA_REMOVED, NA_EDITED);
    }
    return false;
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
    MaterialPaintLayer *from = paint_description_row_for_ordinal(material, from_ordinal);
    MaterialPaintLayer *to = paint_description_row_for_ordinal(material, to_ordinal);
    if (from == nullptr || to == nullptr) {
      return false;
    }
    if (!BKE_paint_layers_edit_reorder(material, *from, *to)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  void add_kinds(Vector<StackAddKindInfo> &r_kinds) const override
  {
    /* The kinds whose features exist: Paint, Fill and Folder. Material and the corrections are
     * appended here as they land, in the order #row_add dispatches by index. */
    r_kinds.append({"PAINT",
                    N_("Paint Layer"),
                    "A layer that shows nothing until it is painted on",
                    ICON_BRUSH_DATA});
    StackAddKindInfo fill{"FILL",
                          N_("Fill Layer"),
                          "A layer of one flat colour",
                          ICON_GP_DRAW_FILL};
    fill.takes_color = true;
    r_kinds.append(fill);
    r_kinds.append({"FOLDER",
                    N_("Folder"),
                    "A group holding other layers, composited on its own",
                    ICON_FILE_FOLDER});
    r_kinds.append({"CORRECTION",
                    N_("Correction Paint"),
                    "A map painted over the active layer's colour, below the layer's own blend",
                    ICON_IMAGE_RGB_ALPHA});
  }

  int row_add(bContext &C,
              const StackFocus & /*focus*/,
              ID &owner,
              const int kind,
              const int ordinal,
              const StackAddArgs &args = {}) const override
  {
    if (kind < 0 || kind > 3 || !this->is_editable(owner)) {
      return -1;
    }
    Material &material = layers_owner(owner);
    MaterialPaintLayer *anchor = (ordinal < 0) ?
                                     nullptr :
                                     paint_description_row_for_ordinal(material, ordinal);
    if (ordinal >= 0 && anchor == nullptr) {
      return -1;
    }
    PaintLayerAddParams params;
    params.kind = (kind == 1) ? PaintLayerAddKind::Fill :
                  (kind == 2) ? PaintLayerAddKind::Folder :
                  (kind == 3) ? PaintLayerAddKind::Correction :
                                PaintLayerAddKind::Paint;
    if (anchor != nullptr && params.kind != PaintLayerAddKind::Correction) {
      /* A correction is not a place a row can be added next to: the row it hangs on is. */
      if (MaterialPaintLayer *parent = BKE_paint_layers_correction_owner(material, *anchor)) {
        anchor = parent;
      }
    }
    if (params.kind == PaintLayerAddKind::Correction && anchor == nullptr) {
      /* No row was named: the correction goes on the one being worked on. */
      anchor = BKE_paint_layers_active_layer_get(material);
      if (anchor == nullptr) {
        BKE_report(CTX_wm_reports(&C), RPT_WARNING, "Select a layer to add a correction to");
        return -1;
      }
    }
    if (params.kind == PaintLayerAddKind::Correction && BKE_paint_layers_is_folder(*anchor)) {
      /* A folder has no colour of its own to correct; saying so beats a button that does nothing. */
      BKE_report(CTX_wm_reports(&C), RPT_WARNING, "A folder takes no correction; select a layer");
      return -1;
    }
    params.anchor = anchor;
    /* An Add onto a folder means filling it: the policy turns this into Into. */
    params.place = PaintLayerPlace::Above;
    if (params.kind == PaintLayerAddKind::Fill && args.color != nullptr) {
      params.fill_color = args.color;
    }
    MaterialPaintLayer *created = BKE_paint_layers_add_with_policy(material, params);
    if (created == nullptr) {
      return -1;
    }
    /* A new layer is painted on its content, not on the mask the previous one was edited in. */
    layers_target_mode_set(C, PAINT_LAYER_TARGET_CONTENT);
    if (params.kind == PaintLayerAddKind::Correction) {
      /* The correction the user just added is the one they mean to paint into. */
      BKE_paint_layers_active_set(material, created->marker);
      layers_texpaint_slots_refresh(C);
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return paint_layers_ordinal_of(material, created);
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
    /* A correction is not a row of the stack: it leaves through its own call. */
    if (BKE_paint_layers_correction_owner(material, *layer) != nullptr) {
      if (!BKE_paint_layers_correction_remove(material, layer)) {
        return false;
      }
      layers_texpaint_slots_refresh(C);
      WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
      return true;
    }
    layers_mask_edit_end_if_active(C, material, *layer);
    if (!BKE_paint_layers_remove(material, layer)) {
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
    MaterialPaintLayer *from = paint_description_row_for_ordinal(material, from_ordinal);
    MaterialPaintLayer *anchor = paint_description_row_for_ordinal(material, anchor_ordinal);
    if (from == nullptr || anchor == nullptr) {
      return false;
    }
    const PaintLayerPlace layer_place = (place == StackMovePlace::Below) ? PaintLayerPlace::Below :
                                        (place == StackMovePlace::Into) ? PaintLayerPlace::Into :
                                                                          PaintLayerPlace::Above;
    if (!BKE_paint_layers_edit_move(material, *from, anchor, layer_place)) {
      if (place == StackMovePlace::Into) {
        /* The drop did nothing; without a word it reads as a dead drag. */
        BKE_report(CTX_wm_reports(&C), RPT_INFO, "Only layers of the same list can be grouped");
      }
      return false;
    }
    /* The ordinals past the move changed; the caller keeps the moved row selected by its new one. */
    if (r_ordinal != nullptr) {
      *r_ordinal = paint_layers_ordinal_of(material, from);
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  /* ---------------------------------------------------------------- */
  /** \name StackGroupingEditor
   *
   * The mask half and the folder verbs; colour tags arrive with their feature, so their verb keeps
   * the default that refuses.
   * \{ */

  const StackGroupingEditor *grouping() const override
  {
    return this;
  }

  StringRefNull aux_section_id() const override
  {
    return "MASK";
  }

  bool row_aux_set(bContext &C,
                   const StackFocus & /*focus*/,
                   ID &owner,
                   const int ordinal,
                   const bool add,
                   const float initial_color[4]) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
    if (layer == nullptr) {
      return false;
    }
    if (!add) {
      layers_mask_edit_end_if_active(C, material, *layer);
      if (!BKE_paint_layers_mask_remove(material, layer)) {
        return false;
      }
      WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
      return true;
    }
    if (BKE_paint_layers_mask_base(*layer) != nullptr) {
      BKE_report(CTX_wm_reports(&C), RPT_WARNING, "This layer already has a mask");
      return false;
    }
    /* The constant is the colour's grey: white shows the row, black hides it until painted in. */
    const float value = (initial_color != nullptr) ? initial_color[0] : 1.0f;
    if (BKE_paint_layers_mask_add(material, layer, value) == nullptr) {
      return false;
    }
    /* A mask the user just added is the one they mean to work on. */
    BKE_paint_layers_active_set(material, layer->marker);
    layers_target_mode_set(C, PAINT_LAYER_TARGET_MASK);
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool row_aux_copy(bContext &C,
                    const StackFocus & /*focus*/,
                    ID &owner,
                    const int source_ordinal,
                    const int target_ordinal) const override
  {
    Material &material = layers_owner(owner);
    Main *bmain = CTX_data_main(&C);
    const MaterialPaintLayer *source = paint_description_row_for_ordinal(material, source_ordinal);
    MaterialPaintLayer *target = paint_description_row_for_ordinal(material, target_ordinal);
    /* A correction has no mask of its own; the one worked on is its layer's. */
    if (target != nullptr) {
      if (MaterialPaintLayer *owner = BKE_paint_layers_correction_owner(material, *target)) {
        target = owner;
      }
    }
    if (bmain == nullptr || source == nullptr || target == nullptr ||
        !BKE_paint_layers_mask_copy(*bmain, material, *source, *target))
    {
      return false;
    }
    layers_texpaint_slots_refresh(C);
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  int row_merge_down(bContext &C,
                     const StackFocus & /*focus*/,
                     ID &owner,
                     const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *upper = paint_description_row_for_ordinal(material, ordinal);
    /* The row below in the compositing sense is the previous one of the same list, not the previous
     * ordinal: that one can be the last row of a folder that sits below. */
    MaterialPaintLayer *lower = (upper != nullptr) ? upper->prev : nullptr;
    if (upper == nullptr || lower == nullptr) {
      return -1;
    }
    MaterialPaintLayer *folder = BKE_paint_layers_edit_merge_down(material, *upper, *lower);
    if (folder == nullptr) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Cannot merge this layer down");
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return paint_layers_ordinal_of(material, folder);
  }

  int rows_group(bContext &C,
                 const StackFocus & /*focus*/,
                 ID &owner,
                 const int from_ordinal,
                 const int to_ordinal) const override
  {
    Material &material = layers_owner(owner);
    const int low = std::min(from_ordinal, to_ordinal);
    const int high = std::max(from_ordinal, to_ordinal);
    if (low < 0) {
      return -1;
    }
    /* The run is by ordinal, which numbers a folder before what it holds: a folder inside the run
     * carries its children along, so they are not listed again. */
    Vector<MaterialPaintLayer *> rows;
    for (int ordinal = low; ordinal <= high; ordinal++) {
      MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
      if (layer == nullptr) {
        return -1;
      }
      /* A correction goes with the row it hangs on. */
      if (BKE_paint_layers_correction_owner(material, *layer) != nullptr) {
        continue;
      }
      /* Any ancestor already listed carries the row, not only its direct parent: a folder nested two
       * deep has its grandchildren in the run too. */
      const bool carried = std::any_of(
          rows.begin(), rows.end(), [&](const MaterialPaintLayer *root) {
            return root != layer && BKE_paint_layers_subtree_contains(*root, layer->marker);
          });
      if (carried) {
        continue;
      }
      rows.append(layer);
    }
    MaterialPaintLayer *folder = BKE_paint_layers_edit_group_range(material, rows);
    if (folder == nullptr) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Only layers of one list can be grouped");
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return paint_layers_ordinal_of(material, folder);
  }

  int row_ungroup(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *folder = paint_description_row_for_ordinal(material, ordinal);
    if (folder == nullptr || !BKE_paint_layers_is_folder(*folder)) {
      return -1;
    }
    const int child_num = BLI_listbase_count(&folder->children);
    layers_mask_edit_end_if_active(C, material, *folder);
    if (!BKE_paint_layers_ungroup(material, folder)) {
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
    MaterialPaintLayer *anchor = (ordinal < 0) ?
                                     nullptr :
                                     paint_description_row_for_ordinal(material, ordinal);
    if (ordinal >= 0 && anchor == nullptr) {
      return -1;
    }
    MaterialPaintLayer *folder = BKE_paint_layers_edit_group_add(material, anchor);
    if (folder == nullptr) {
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return paint_layers_ordinal_of(material, folder);
  }

  bool row_aux_toggle(bContext &C,
                      const StackFocus & /*focus*/,
                      ID &owner,
                      const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
    MaterialPaintLayer *base = (layer != nullptr) ? BKE_paint_layers_mask_base(*layer) : nullptr;
    if (base == nullptr) {
      return false;
    }
    const bool enable = (base->flag & MA_PAINT_LAYER_ENABLED) == 0;
    if (!enable) {
      /* A switched-off mask hides nothing, so a stroke into it would be invisible. */
      layers_mask_edit_end_if_active(C, material, *layer);
    }
    if (!BKE_paint_layers_set_enabled(material, base, enable)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  /** \} */

  bool row_set_enabled(bContext &C,
                       const StackFocus & /*focus*/,
                       ID &owner,
                       const int ordinal,
                       const bool enable) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
    if (layer == nullptr || !BKE_paint_layers_set_enabled(material, layer, enable)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
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

  /** \} */
};

std::unique_ptr<StackSource> stack_source_paint_layers_create()
{
  return std::make_unique<PaintLayersStackSource>();
}

}  // namespace blender::ed::outliner
