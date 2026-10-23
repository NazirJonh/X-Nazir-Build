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
#include <optional>
#include <string>
#include <type_traits>

#include <fmt/format.h>

#include "AS_asset_representation.hh"

#include "DNA_ID.h"
#include "DNA_asset_types.h"
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
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_object_mask.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_material_sync.hh"
#include "BKE_report.hh"

#include "BLI_assert.h"
#include "BLI_fileops.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "IMB_imbuf_types.hh"

#include "ED_asset_image_utils.hh"
#include "ED_asset_import.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_types.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "outliner_stack_layers_intern.hh"
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

/**
 * The channel the Stack Layers header selected, from the scene's paint settings; clamped.
 *
 * The cursor is a scene setting while the channel set belongs to the material, so it can name a
 * channel \a material leaves out (Base Color by default, or after switching to another material).
 * The columns would then edit a channel that takes part nowhere, and the header menu, which only
 * offers the set, would still list it. The cursor is moved into the set here, the one place every
 * view of the stack passes through, so the header and the columns always agree.
 */
int paint_stack_selected_channel(const StackReadContext &ctx, const Material &material)
{
  if (ctx.scene == nullptr || ctx.scene->toolsettings == nullptr) {
    return PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  }
  int &cursor = ctx.scene->toolsettings->paint_mode.stack_layer_channel;
  const int clamped = BKE_paint_layers_channel_clamp_to_set(
      material,
      (cursor >= 0 && cursor < PAINT_MATERIAL_CHANNEL_NUM) ? cursor :
                                                              PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  cursor = clamped;
  return clamped;
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
  /* Every change of the active row, added row or mode passes here, so the PBR Paint tab never
   * waits for a separate opt-in button while a layer is active. */
  if (Paint *paint = BKE_paint_get_active_from_context(&C)) {
    if (BKE_paint_material_brush_auto_ensure(scene, paint, object)) {
      WM_event_add_notifier(&C, NC_BRUSH | NA_EDITED, BKE_paint_brush(paint));
    }
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
  /* Choosing a paint target (Mask or content) ends object-mask editing, even when the target is
   * already the one asked for. */
  if (paint_mode.layer_object_mask_edit != 0) {
    paint_mode.layer_object_mask_edit = 0;
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
  }
  if (paint_mode.layer_target_mode == mode) {
    return false;
  }
  const bool leaving_mask = paint_mode.layer_target_mode == PAINT_LAYER_TARGET_MASK &&
                            mode != PAINT_LAYER_TARGET_MASK;
  if (Paint *paint = BKE_paint_get_active_from_context(&C)) {
    /* Puts the Mask brush in hand on the way in and the remembered content brush on the way out. */
    BKE_paint_material_layer_target_mode_set(
        *CTX_data_main(&C), *scene, *paint, paint_mode, mode);
    if (leaving_mask && paint->brush != nullptr) {
      /* The restored content brush may belong to another tool, so that tool becomes active too. */
      WM_toolsystem_activate_brush_and_tool(&C, paint, paint->brush);
    }
  }
  else {
    paint_mode.layer_target_mode = int8_t(mode);
  }
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

/**
 * The description's own problems -- a folder carrying a map, a Fill correction on Normal -- shown on
 * the row they are about, as a warning icon with the reason in its tooltip. \a correction is the
 * correction the issue is about, or nil for the layer's own.
 */
void paint_issue_slots_append(const Span<PaintLayersIssue> issues,
                              const bUUID &layer,
                              const bUUID &correction,
                              StackRow &row)
{
  for (const PaintLayersIssue &issue : issues) {
    if (!BLI_uuid_equal(issue.layer, layer) || !BLI_uuid_equal(issue.correction, correction)) {
      continue;
    }
    StackRowPreview warning;
    warning.icon = ICON_ERROR;
    warning.label = issue.text != nullptr ? issue.text : "";
    row.preview_slots.append(std::move(warning));
  }
}

/**
/**
 * Point the value column at the per-channel opacity of \a layer for \a channel, so the header's
 * channel cursor edits that channel alone. Every row has one, a folder and a correction included,
 * whether or not it has a record for the channel.
 */
void paint_row_value_target_set(StackRow &row,
                                const Material &material,
                                const MaterialPaintLayer &layer,
                                const int channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return;
  }
  const MaterialPaintLayerChannelSettings &settings = layer.channel_settings[channel];
  row.value_ptr = RNA_pointer_create_discrete(
      &const_cast<Material &>(material).id,
      RNA_struct_find("MaterialPaintLayerChannelSettings"),
      const_cast<MaterialPaintLayerChannelSettings *>(&settings));
  row.value_prop = "opacity";
  row.value_inherited = (settings.opacity == 1.0f);
}

/**
 * Point the mode column at the per-channel blend of \a layer for \a channel, so the header's
 * channel cursor edits that channel alone. Inherit is the row's own blend, so the control dims
 * until the user picks another mode; the Normal channel's Mix is its combine rather than an
 * inherited value, so it stays bright.
 */
void paint_row_mode_target_set(StackRow &row,
                               const Material &material,
                               const MaterialPaintLayer &layer,
                               const int channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return;
  }
  const MaterialPaintLayerChannelSettings &settings = layer.channel_settings[channel];
  row.mode_ptr = RNA_pointer_create_discrete(
      &const_cast<Material &>(material).id,
      RNA_struct_find("MaterialPaintLayerChannelSettings"),
      const_cast<MaterialPaintLayerChannelSettings *>(&settings));
  row.mode_prop = "blend_type";
  row.mode_inherited = settings.blend < 0 && channel != PAINT_MATERIAL_CHANNEL_NORMAL;
}


/** Whether \a layer is an adjustment: a filter that is a row of the stack. */
static bool paint_layer_row_is_adjustment_view(const MaterialPaintLayer &layer)
{
  return layer.source == MA_PAINT_LAYER_SOURCE_FILTER && layer.role == MA_PAINT_LAYER_ROLE_LAYER;
}

/**
 * A filter the Add Effect menu offers. The vocabulary is the editor's (a name, an icon, a tooltip);
 * which filters exist is #eMaterialPaintLayerFilter. #row_add reads the position back, so a new
 * filter is one entry here and one in the kernel's kind table.
 */
struct PaintFilterAddKind {
  const char *identifier;
  const char *name;
  const char *description;
  int icon;
  int16_t filter;
};

static const PaintFilterAddKind paint_filter_add_kinds[] = {
    {"FILTER_INVERT",
     N_("Invert"),
     "Invert the active layer's colour, by the filter's opacity",
     ICON_ARROW_LEFTRIGHT,
     MA_PAINT_LAYER_FILTER_INVERT},
    {"FILTER_LEVELS",
     N_("Levels"),
     "Remap the active layer's tones: black and white points, gamma and output range",
     ICON_IMAGE_ALPHA,
     MA_PAINT_LAYER_FILTER_LEVELS},
    {"FILTER_HSL",
     N_("Hue Saturation Value"),
     "Shift the active layer's hue, saturation and value",
     ICON_COLOR,
     MA_PAINT_LAYER_FILTER_HSL},
};

/**
 * The map-driven filters the Add Effect menu offers over the mask. They only exist in a mask (their
 * role is a mask item), so they have no correction or adjustment form like #paint_filter_add_kinds.
 */
static const PaintFilterAddKind paint_coverage_add_kinds[] = {
    {"FILTER_COLOR_ID",
     N_("Color ID"),
     "Keep the active layer's mask only where an ID Map matches the listed colors",
     ICON_COLOR,
     MA_PAINT_LAYER_FILTER_COLOR_ID},
    {"FILTER_UV_ISLAND",
     N_("UV Island"),
     "Keep the active layer's mask only on the UV islands captured from the Image Editor",
     ICON_UV_ISLANDSEL,
     MA_PAINT_LAYER_FILTER_UV_ISLAND},
};

/** The row of one content correction, hung off the row \a owner_ordinal names. */
StackRow paint_correction_row_build(const Material &material,
                                    const MaterialPaintLayer &effect,
                                    const bUUID &owner_marker,
                                    const Span<PaintLayersIssue> issues,
                                    const int depth,
                                    const int owner_ordinal,
                                    const int ordinal,
                                    const int channel)
{
  StackRow row;
  row.ordinal = int16_t(ordinal);
  row.depth = depth;
  row.parent_ordinal = int16_t(owner_ordinal);
  row.parent_section_id = "CHANNELS";
  row.stable_id = effect.marker;
  row.enabled = (effect.flag & MA_PAINT_LAYER_ENABLED) != 0;
  row.supported = true;
  const bool fill = effect.source == MA_PAINT_LAYER_SOURCE_CONSTANT;
  const bool from_material = effect.source == MA_PAINT_LAYER_SOURCE_MATERIAL;
  row.name = effect.name[0] != '\0' ?
                 effect.name :
                 (from_material ? "Material Correction" :
                                  (fill ? "Correction Fill" : "Correction Paint"));
  row.name_buffer = const_cast<char *>(effect.name);
  const bool filter = effect.source == MA_PAINT_LAYER_SOURCE_FILTER;
  row.icon = filter        ? ICON_ARROW_LEFTRIGHT :
             from_material ? ICON_MATERIAL :
             fill          ? ICON_GP_DRAW_FILL :
                             ICON_BRUSH_DATA;
  if (BKE_paint_layers_is_anchor_point(effect)) {
    /* A marker is a place, not a correction: it publishes a slice and has no opacity, blend or map
     * of its own, so it is a bare compact row with the icon of its Add entry. */
    row.name = effect.name[0] != '\0' ? effect.name : "Anchor Point";
    row.icon = ICON_EMPTY_AXIS;
    row.compact = true;
    paint_issue_slots_append(issues, owner_marker, effect.marker, row);
    return row;
  }
  const bool reads_anchor_point = BKE_paint_layers_reads_anchor_point(effect);
  if (reads_anchor_point) {
    row.name = effect.name[0] != '\0' ? effect.name : "Anchor Point Reader";
    row.icon = ICON_LINKED;
  }
  /* A correction reads as a setting of its layer rather than as a member of the stack. */
  row.compact = true;

  PointerRNA effect_ptr = RNA_pointer_create_discrete(
      &const_cast<Material &>(material).id,
      RNA_struct_find("MaterialPaintLayer"),
      const_cast<MaterialPaintLayer *>(&effect));
  row.value_ptr = effect_ptr;
  row.value_prop = "opacity";
  row.value_inherited = (effect.opacity == 1.0f);
  paint_row_value_target_set(row, material, effect, channel);
  if (filter) {
    /* A filter has no map to paint into and no blend: only its opacity and its issues. */
    paint_issue_slots_append(issues, owner_marker, effect.marker, row);
    return row;
  }
  /* The mode column edits the blend of this channel alone, like the value column above. */
  paint_row_mode_target_set(row, material, effect, channel);
  if (from_material || reads_anchor_point) {
    /* It shows what its source shows and owns no map: nothing to preview or switch the brush to,
     * as for a Material layer. A reader shows the slice its point publishes. */
    paint_issue_slots_append(issues, owner_marker, effect.marker, row);
    return row;
  }

  /* The slot shows the correction's first map, blank or not, and switches the brush to content. A
   * Fill correction is its colour while it has no map: the swatch a Fill layer shows, with the same
   * rule when the Base Color record is gone or switched off (the correction then lays no colour). */
  StackRowPreview slot;
  slot.section_id = "CHANNELS";
  const MaterialPaintLayerChannel *base = nullptr;
  for (int c = 0; c < effect.channels_num; c++) {
    if (effect.channels[c].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      base = &effect.channels[c];
    }
  }
  if (fill && BKE_paint_layers_base_color_active(effect) && base->image == nullptr &&
      BKE_paint_layers_channel_in_set(material, PAINT_MATERIAL_CHANNEL_BASE_COLOR))
  {
    slot.is_color_swatch = true;
    slot.icon = ICON_GP_DRAW_FILL;
    BKE_paint_layers_base_color_get(effect, slot.color);
    slot.label = IFACE_("Fill Color");
  }
  for (int c = 0; c < effect.channels_num && !slot.is_color_swatch; c++) {
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
  paint_issue_slots_append(issues, owner_marker, effect.marker, row);
  return row;
}

/**
 * The row of one mask correction, hung off the row \a owner_ordinal names under its mask section: it
 * is listed while the mask is the content on show, the way a content correction is while the
 * channels are.
 */
StackRow paint_mask_item_row_build(const Material &material,
                                   const MaterialPaintLayer &item,
                                   const bUUID &owner_marker,
                                   const int depth,
                                   const int owner_ordinal,
                                   const int ordinal,
                                   const Span<PaintLayersIssue> issues)
{
  const bool fill = item.source == MA_PAINT_LAYER_SOURCE_CONSTANT;
  const bool from_material = item.source == MA_PAINT_LAYER_SOURCE_MATERIAL;
  StackRow row;
  row.ordinal = int16_t(ordinal);
  row.depth = depth;
  row.parent_ordinal = int16_t(owner_ordinal);
  row.parent_section_id = "MASK";
  row.stable_id = item.marker;
  row.enabled = (item.flag & MA_PAINT_LAYER_ENABLED) != 0;
  row.supported = true;
  row.name = item.name[0] != '\0' ?
                 item.name :
                 (from_material ? "Material Mask" : (fill ? "Mask Fill" : "Mask Paint"));
  row.name_buffer = const_cast<char *>(item.name);
  row.icon = from_material ? ICON_MATERIAL : (fill ? ICON_GP_DRAW_FILL : ICON_MOD_MASK);
  if (BKE_paint_layers_is_anchor_point(item)) {
    row.name = item.name[0] != '\0' ? item.name : "Anchor Point";
    row.icon = ICON_EMPTY_AXIS;
    row.compact = true;
    paint_issue_slots_append(issues, owner_marker, item.marker, row);
    return row;
  }
  const bool reads_anchor_point = BKE_paint_layers_reads_anchor_point(item);
  if (reads_anchor_point) {
    row.name = item.name[0] != '\0' ? item.name : "Anchor Point Reader";
    row.icon = ICON_LINKED;
  }
  /* A correction reads as a setting of its mask rather than as a member of the stack. */
  row.compact = true;

  PointerRNA item_ptr = RNA_pointer_create_discrete(
      &const_cast<Material &>(material).id,
      RNA_struct_find("MaterialPaintLayer"),
      const_cast<MaterialPaintLayer *>(&item));
  row.value_ptr = item_ptr;
  row.value_prop = "opacity";
  row.value_inherited = (item.opacity == 1.0f);
  if (item.source == MA_PAINT_LAYER_SOURCE_FILTER) {
    /* A filter in the mask changes its coverage: no map to paint, no blend, no channels. */
    row.icon = ICON_ARROW_LEFTRIGHT;
    /* The map-driven filters read as what they select by, not as a tone change. */
    if (item.kind_subtype == MA_PAINT_LAYER_FILTER_COLOR_ID) {
      row.icon = ICON_COLOR;
    }
    else if (item.kind_subtype == MA_PAINT_LAYER_FILTER_UV_ISLAND) {
      row.icon = ICON_UV_ISLANDSEL;
    }
    paint_issue_slots_append(issues, owner_marker, item.marker, row);
    return row;
  }
  row.mode_ptr = item_ptr;
  row.mode_prop = "blend_type";
  row.mode_inherited = (item.blend == MA_PAINT_LAYER_BLEND_MIX);
  if (from_material || reads_anchor_point) {
    /* Its grey is its source's Base Color (or the slice it reads) and it owns no map: nothing to
     * preview or paint. A reader's issues (a bad reference, a UDIM slice) are shown here. */
    paint_issue_slots_append(issues, owner_marker, item.marker, row);
    return row;
  }

  /* The slot switches the brush to the mask. A Fill is its grey while it has no map, as a Fill layer
   * is its colour; a Paint one, and a Fill once painted, is the mask icon the base's slot shows. */
  StackRowPreview slot;
  slot.section_id = "MASK";
  slot.label = IFACE_("Mask");
  const MaterialPaintLayerChannel *record = paint_mask_record(item);
  if (fill && record != nullptr && record->image == nullptr) {
    const float gray = BKE_paint_layers_mask_constant(item);
    slot.is_color_swatch = true;
    slot.color[0] = slot.color[1] = slot.color[2] = gray;
    slot.color[3] = 1.0f;
    slot.icon = ICON_MOD_MASK;
  }
  else {
    slot.icon = row.enabled ? ICON_MOD_MASK : ICON_CLIPUV_HLT;
  }
  row.preview_slots.append(std::move(slot));
  StackContentSection mask_section;
  mask_section.identifier = "MASK";
  mask_section.name = IFACE_("Mask Content");
  row.content_sections.append(std::move(mask_section));
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
                             const Span<PaintLayersIssue> issues,
                             const int depth,
                             const int parent_ordinal,
                             const int channel,
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
    row.color_tag = layer.color_tag;
    /* An adjustment is a filter of the stack below it: no map, no channel slot, only its settings. */
    const bool adjustment = paint_layer_row_is_adjustment_view(layer);
    row.icon = folder                                         ? ICON_FILE_FOLDER :
               adjustment                                     ? ICON_ARROW_LEFTRIGHT :
               (layer.source == MA_PAINT_LAYER_SOURCE_CONSTANT) ? ICON_GP_DRAW_FILL :
               (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) ? ICON_MATERIAL :
                                                                  ICON_IMAGE_RGB;
    /* Only layers and folders limit where they show; an adjustment acts on what is below it for
     * every object alike, and corrections and mask items are not rows of this list. */
    if (!adjustment && layer.role == MA_PAINT_LAYER_ROLE_LAYER) {
      row.action_icon = ICON_SELECT_SET;
      row.action_tooltip = TIP_("Mesh Mask: choose the objects or UDIM tiles this layer shows on");
    }

    /* The value column edits the row's opacity; a value left at its default reads as inherited
     * rather than deliberately set, so the control dims until the user moves it. */
    PointerRNA layer_ptr = RNA_pointer_create_discrete(
        &const_cast<Material &>(material).id,
        RNA_struct_find("MaterialPaintLayer"),
        const_cast<MaterialPaintLayer *>(&layer));
    row.value_ptr = layer_ptr;
    row.value_prop = "opacity";
    row.value_inherited = (layer.opacity == 1.0f);
    paint_row_value_target_set(row, material, layer, channel);

    /* The mode column edits how the row blends over what is below it in the selected channel;
     * Inherit is the row's own blend, so the control dims until the user picks another mode. */
    paint_row_mode_target_set(row, material, layer, channel);

    if (folder) {
      /* A folder is what it holds: it has no map, so no channel slot. Its content section is the
       * one the brush switches back to from the mask. */
      StackContentSection content;
      content.identifier = "CHANNELS";
      content.name = IFACE_("Content");
      row.content_sections.append(std::move(content));
    }
    else if (adjustment || layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
      /* Nothing to show or switch to: the brush has no content to work on in this row. A Material
       * row shows what its source shows and owns no map to paint. With a mask the row does have
       * two targets, and the slot back to the content (made below) needs its section. A Material
       * row takes corrections, which are listed under this section, so it has it with or without
       * a mask. */
      if (BKE_paint_layers_mask_base(layer) != nullptr ||
          layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL)
      {
        StackContentSection content;
        content.identifier = "CHANNELS";
        content.name = IFACE_("Content");
        row.content_sections.append(std::move(content));
      }
    }
    else if (layer.source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
      /* A Fill is its colour: one swatch with the bucket drawn over it, and no channel-texture
       * placeholder. With the Base Color record removed, switched off, or outside the material's
       * channel set the row lays no colour, so no swatch is drawn. */
      if (BKE_paint_layers_base_color_active(layer) &&
          BKE_paint_layers_channel_in_set(material, PAINT_MATERIAL_CHANNEL_BASE_COLOR))
      {
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

    if (!folder && !adjustment && layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL) {
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
      if (folder || adjustment || layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
        /* The slot that switches back to the content, ahead of the mask's. The row icon stays in
         * front of it, as it does for a folder with no mask; these rows have no map of their own to
         * show in a slot, so without this one the brush could not leave the mask. */
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

    paint_issue_slots_append(issues, layer.marker, bUUID{}, row);

    const int row_ordinal = row.ordinal;
    /* The corrections are dense settings of their layer: rows attached to its content section,
     * numbered right after it (see #paint_rows_walk). */
    for (const MaterialPaintLayer &effect : layer.effects) {
      if (ordinal > STACK_ROW_ORDINAL_MAX) {
        overflow = true;
        return;
      }
      r_rows.append(paint_correction_row_build(
          material, effect, layer.marker, issues, depth + 1, row_ordinal, ordinal++, channel));
    }
    /* The mask's corrections follow, numbered the same way: the base is the mask's own slot. */
    for (const MaterialPaintLayer &item : layer.mask_stack) {
      if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
        continue;
      }
      if (ordinal > STACK_ROW_ORDINAL_MAX) {
        overflow = true;
        return;
      }
      r_rows.append(paint_mask_item_row_build(
          material, item, layer.marker, depth + 1, row_ordinal, ordinal++, issues));
    }
    if (folder) {
      paint_stack_rows_append(
          material, layer.children, issues, depth + 1, row_ordinal, channel, ordinal, overflow, r_rows);
    }
    r_rows.append(std::move(row));
  }
}

}  // namespace

void paint_stack_rows_from_description(const Material &material,
                                       const int channel,
                                       Vector<StackRow> &r_rows)
{

  int ordinal = 0;
  bool overflow = false;
  /* Built once: it is cheap, but a row would otherwise ask the whole description per row. */
  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(material, issues);
  paint_stack_rows_append(
      material, material.paint_layers, issues, 0, -1, channel, ordinal, overflow, r_rows);
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
 * Visit every row the Outliner lists in the order it numbers them: a row, then its corrections, then
 * its mask's, then what a folder holds. #paint_stack_rows_append hands out the ordinals in this same order, so
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
    for (RowT &item : layer.mask_stack) {
      if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
        continue;
      }
      if (!fn(item)) {
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

bool paint_layers_edit_color_tag(Material &material, const int ordinal, const int color_tag)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  return layer != nullptr && BKE_paint_layers_set_color_tag(material, layer, color_tag);
}

bool paint_layers_edit_target_clear(Material &material)
{
  if (BLI_uuid_is_nil(material.active_layer_marker)) {
    return false;
  }
  BKE_paint_layers_active_set(material, bUUID{});
  return true;
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
    hash = hash * 1000003u ^ uint64_t(paint_stack_selected_channel(ctx, material));
    /* The set decides the swatches and the cursor's stand-in, and an edit of it moves no marker. */
    hash = hash * 1000003u ^ uint64_t(BKE_paint_layers_channel_set_mask_get(material));
    /* The action button reads pressed from this flag, so the rows have to rebuild when it flips. */
    if (ctx.scene != nullptr && ctx.scene->toolsettings != nullptr) {
      hash = hash * 1000003u ^
             uint64_t(ctx.scene->toolsettings->paint_mode.layer_object_mask_edit != 0);
    }
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
      /* The group's folder icon and hierarchy line are drawn from the tag, so a change to it has
       * to reach the rows even though nothing evaluated reads it. */
      hash = hash * 1000003u ^ uint64_t(uint8_t(layer.color_tag));
      /* The swatch and the columns show these, so a change to them has to reach the rows. */
      hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(layer.opacity));
      for (const MaterialPaintLayerChannelSettings &settings : layer.channel_settings) {
        hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(settings.opacity));
        hash = hash * 1000003u ^ uint64_t(uint8_t(settings.blend));
      }
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
      /* A mask correction is a row of its own too: its blend, opacity, name and grey or map. */
      for (const MaterialPaintLayer &item : layer.mask_stack) {
        if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
          continue;
        }
        hash = hash * 1000003u ^ UUID(item.marker).hash();
        hash ^= uint64_t(item.flag);
        hash = hash * 1000003u ^ uint64_t(item.blend);
        hash ^= uint64_t(item.source) << 16;
        hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(item.opacity));
        for (const char *ch = item.name; *ch != '\0'; ch++) {
          hash = hash * 131u ^ uint64_t(uint8_t(*ch));
        }
        if (const MaterialPaintLayerChannel *record = paint_mask_record(item)) {
          hash = hash * 1000003u ^
                 uint64_t(std::hash<float>{}(BKE_paint_layers_mask_constant(item)));
          if (record->image != nullptr) {
            hash ^= uint64_t(record->image->id.session_uid) * 2654435761u;
            hash = hash * 1000003u ^ uint64_t(paint_image_is_blank(*record->image) ? 1 : 0);
          }
        }
      }
      /* The corrections are rows of their own: what their rows show has to reach the tree too. */
      for (const MaterialPaintLayer &effect : layer.effects) {
        hash = hash * 1000003u ^ UUID(effect.marker).hash();
        hash ^= uint64_t(effect.flag);
        hash = hash * 1000003u ^ uint64_t(effect.blend);
        hash ^= uint64_t(effect.source) << 16;
        hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(effect.opacity));
        for (const MaterialPaintLayerChannelSettings &settings : effect.channel_settings) {
          hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(settings.opacity));
          hash = hash * 1000003u ^ uint64_t(uint8_t(settings.blend));
        }
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
          else if (effect.source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
            /* The swatch shows the colour, so an edit of it has to reach the row. */
            for (const int k : IndexRange(4)) {
              hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(effect.channels[c].value[k]));
            }
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
    const Material &material = layers_owner(owner);
    paint_stack_rows_from_description(
        material, paint_stack_selected_channel(ctx, material), r_rows);
    if (ctx.scene != nullptr && ctx.scene->toolsettings != nullptr &&
        ctx.scene->toolsettings->paint_mode.layer_object_mask_edit != 0)
    {
      for (StackRow &row : r_rows) {
        row.action_active = row.action_icon != ICON_NONE && !BLI_uuid_is_nil(row.stable_id) &&
                            BLI_uuid_equal(material.active_layer_marker, row.stable_id);
      }
    }
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
    layout.action_width = 1.0f;
    return layout;
  }

  bool row_action(bContext &C,
                  ID &owner,
                  const int ordinal,
                  const StackRow &row) const override
  {
    Scene *scene = CTX_data_scene(&C);
    SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
    if (scene == nullptr || scene->toolsettings == nullptr || space_outliner == nullptr) {
      return false;
    }
    PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
    if (row.action_active) {
      paint_mode.layer_object_mask_edit = 0;
    }
    else {
      /* Activating may switch the target mode, which clears the flag -- so set it after. The
       * activation changes data (the active row), so it gets an undo step like the activate
       * operator does. */
      const Material &material = layers_owner(owner);
      const bool active_changed = !BLI_uuid_equal(material.active_layer_marker, row.stable_id);
      outliner_stack_row_activate(&C, *space_outliner, ordinal);
      paint_mode.layer_object_mask_edit = 1;
      /* Scan the UVs once now, so the Properties redraws only read the tile cache. */
      BKE_paint_layer_object_mask_tiles_prefetch(*CTX_data_main(&C));
      if (active_changed) {
        ED_undo_push(&C, "Activate Paint Layer");
      }
    }
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
    return true;
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
    /* Mask editing only makes sense on a Layer row; leaving it for a correction or mask item ends
     * it. */
    if (Scene *scene = CTX_data_scene(&C)) {
      const MaterialPaintLayer *active = BKE_paint_layers_find(material, row.stable_id);
      if (scene->toolsettings != nullptr && active != nullptr &&
          active->role != MA_PAINT_LAYER_ROLE_LAYER &&
          scene->toolsettings->paint_mode.layer_object_mask_edit != 0)
      {
        scene->toolsettings->paint_mode.layer_object_mask_edit = 0;
        WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
      }
    }
    /* The mode is global: a layer with no mask is painted on its content, not left as a stroke
     * into nothing, and a mask correction is only ever worked on in the mask. */
    const MaterialPaintLayer *activated = BKE_paint_layers_active_layer_get(material);
    if (BKE_paint_layers_active_mask_item_get(material) != nullptr) {
      layers_target_mode_set(C, PAINT_LAYER_TARGET_MASK);
    }
    else if (activated != nullptr && BKE_paint_layers_mask_base(*activated) == nullptr) {
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

  /**
   * Stop aiming strokes at the row the stack was activated to. The target is
   * #Material::active_layer_marker, so dropping the marker is the whole edit; the mask-edit cursor
   * goes back to content because a mask belonging to a row that is no longer the target is not
   * something a stroke should keep writing into.
   */
  bool target_clear(bContext &C) const override
  {
    /* The owner the Outliner is focused on, not the active object: the operator acts on the stack
     * being shown. */
    SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
    if (space_outliner == nullptr) {
      return false;
    }
    ID *owner = outliner_stack_owner_get(outliner_stack_read_context(C), *space_outliner);
    if (owner == nullptr || GS(owner->name) != ID_MA) {
      return false;
    }
    Material &material = layers_owner(*owner);
    if (!paint_layers_edit_target_clear(material)) {
      return false;
    }
    /* Back to the neutral target: #row_activate may have put the mode in Mask, and an armed
     * object-mask edit belongs to the row that is no longer the target. This also ends Mask mode
     * and restores the content brush when it does. */
    layers_target_mode_set(C, PAINT_LAYER_TARGET_CONTENT);
    /* Even when the mode already was Content, the slots held the dropped row's maps. */
    layers_texpaint_slots_refresh(C);
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  std::optional<eObjectMode> focus_object_mode() const override
  {
    return OB_MODE_SCULPT;
  }

  const char *focus_tool_idname() const override
  {
    return "builtin_brush.paint";
  }

  const StackDropHandler *drop_handler() const override
  {
    return drop_handler_.get();
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
    /* The kinds whose features exist: Paint, Fill, Folder and the two corrections. The others are
     * appended here as they land, in the order #row_add dispatches by index; the Material layer
     * comes after the last filter. */
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
    StackAddKindInfo correction{
        "CORRECTION",
        N_("Correction Paint"),
        "A map painted over the active layer's colour, below the layer's own blend",
        ICON_BRUSH_DATA};
    correction.is_effect = true;
    r_kinds.append(correction);
    StackAddKindInfo correction_fill{
        "CORRECTION_FILL",
        N_("Correction Fill"),
        "A flat colour laid over the active layer's colour, below the layer's own blend",
        ICON_GP_DRAW_FILL};
    correction_fill.takes_color = true;
    correction_fill.is_effect = true;
    r_kinds.append(correction_fill);
    StackAddKindInfo mask_correction{
        "MASK_CORRECTION",
        N_("Mask Paint Correction"),
        "A grey map painted over the active layer's mask, in Mask mode",
        ICON_MOD_MASK};
    mask_correction.is_effect = true;
    r_kinds.append(mask_correction);
    StackAddKindInfo mask_correction_fill{"MASK_CORRECTION_FILL",
                                          N_("Mask Fill Correction"),
                                          "A flat grey laid over the active layer's mask",
                                          ICON_GP_DRAW_FILL};
    mask_correction_fill.takes_color = true;
    mask_correction_fill.is_effect = true;
    r_kinds.append(mask_correction_fill);
    /* The filters follow, in the order of #paint_filter_add_kinds: #row_add reads the position back
     * as an index into it. */
    for (const PaintFilterAddKind &filter : paint_filter_add_kinds) {
      StackAddKindInfo info{filter.identifier, filter.name, filter.description, filter.icon};
      info.is_effect = true;
      r_kinds.append(info);
    }
    /* Then the same filters as rows of the stack, in the same order: an adjustment changes the whole
     * stack below it instead of one layer's colour. */
    for (const PaintFilterAddKind &filter : paint_filter_add_kinds) {
      r_kinds.append({std::string("ADJUSTMENT") + (filter.identifier + std::strlen("FILTER")),
                      std::string(filter.name) + " Adjustment",
                      "Apply the filter to everything below this layer, with the layer's own "
                      "opacity, blend and mask",
                      filter.icon});
    }
    /* And once more in the mask: the filter changes the mask's coverage, a single grey, so it has no
     * channels to choose. They are corrections of the mask, listed with the other effects. */
    for (const PaintFilterAddKind &filter : paint_filter_add_kinds) {
      StackAddKindInfo info{std::string("MASK_") + filter.identifier,
                            std::string("Mask ") + filter.name,
                            "Apply the filter to the active layer's mask coverage",
                            ICON_MOD_MASK};
      info.is_effect = true;
      r_kinds.append(info);
    }
    /* Last, the map-driven filters of the mask, in the order of #paint_coverage_add_kinds. */
    for (const PaintFilterAddKind &filter : paint_coverage_add_kinds) {
      StackAddKindInfo info{std::string("MASK_") + filter.identifier,
                            std::string("Mask ") + filter.name,
                            filter.description,
                            filter.icon};
      info.is_effect = true;
      r_kinds.append(info);
    }
    /* Last of all, so no index above moves: a layer that shows what another material shows. */
    StackAddKindInfo material{"MATERIAL",
                              N_("Material Layer"),
                              "A layer that shows what another material shows",
                              ICON_MATERIAL};
    material.source_id_type = ID_MA;
    /* Picked from the ID Browser popover, with the Material asset libraries. */
    material.source_browse_prop = "paint_layer_add_material";
    r_kinds.append(material);
    /* After it again, for the same reason: a correction that lays what another material shows. */
    StackAddKindInfo material_correction{
        "CORRECTION_MATERIAL",
        N_("Material Correction"),
        "What another material shows, laid over the active layer's colour, below the layer's own "
        "blend",
        ICON_MATERIAL};
    material_correction.source_id_type = ID_MA;
    /* Picked from the same ID Browser popover as a Material layer. */
    material_correction.source_browse_prop = "paint_layer_add_material_correction";
    material_correction.is_effect = true;
    r_kinds.append(material_correction);
    /* And once more for the mask: the grey of what another material shows as its Base Color. */
    StackAddKindInfo mask_material{
        "MASK_CORRECTION_MATERIAL",
        N_("Mask Material Correction"),
        "The grey of another material's Base Color, laid over the active layer's mask",
        ICON_MATERIAL};
    mask_material.source_id_type = ID_MA;
    /* Picked from the same ID Browser popover as a Material layer. */
    mask_material.source_browse_prop = "paint_layer_add_material_mask_correction";
    mask_material.is_effect = true;
    r_kinds.append(mask_material);
    /* Anchor Points last of all, so no index above moves: the marker and the three places a
     * reader can land. A marker picks its list from the active row; a reader's item decides the
     * list, the way the three Material items do. */
    StackAddKindInfo anchor_point{
        "ANCHOR_POINT",
        N_("Anchor Point"),
        "Publish the active layer's content in its Effects up to this place; select a mask item "
        "first to put the point in the mask instead",
        ICON_EMPTY_AXIS};
    anchor_point.is_effect = true;
    r_kinds.append(anchor_point);
    StackAddKindInfo anchor_point_layer{
        "ANCHOR_POINT_LAYER",
        N_("Anchor Point Layer"),
        "A layer that shows the slice an Anchor Point of this material publishes",
        ICON_LINKED};
    r_kinds.append(anchor_point_layer);
    StackAddKindInfo anchor_point_correction{
        "ANCHOR_POINT_CORRECTION",
        N_("Anchor Point Correction"),
        "The slice an Anchor Point publishes, laid over the active layer's colour, below the "
        "layer's own blend",
        ICON_LINKED};
    anchor_point_correction.is_effect = true;
    r_kinds.append(anchor_point_correction);
    StackAddKindInfo anchor_point_mask_correction{
        "ANCHOR_POINT_MASK_CORRECTION",
        N_("Anchor Point Mask Correction"),
        "The slice an Anchor Point publishes, laid over the active layer's mask",
        ICON_LINKED};
    anchor_point_mask_correction.is_effect = true;
    r_kinds.append(anchor_point_mask_correction);
  }

  int row_add(bContext &C,
              const StackFocus & /*focus*/,
              ID &owner,
              const int kind,
              const int ordinal,
              const StackAddArgs &args = {}) const override
  {
    constexpr int filter_first_kind = 7;
    constexpr int filter_num = int(std::size(paint_filter_add_kinds));
    constexpr int adjustment_first_kind = filter_first_kind + filter_num;
    constexpr int mask_filter_first_kind = adjustment_first_kind + filter_num;
    constexpr int coverage_first_kind = mask_filter_first_kind + filter_num;
    constexpr int coverage_num = int(std::size(paint_coverage_add_kinds));
    constexpr int material_kind = coverage_first_kind + coverage_num;
    constexpr int material_correction_kind = material_kind + 1;
    constexpr int mask_material_kind = material_correction_kind + 1;
    constexpr int anchor_point_kind = mask_material_kind + 1;
    constexpr int anchor_point_layer_kind = anchor_point_kind + 1;
    constexpr int anchor_point_correction_kind = anchor_point_layer_kind + 1;
    constexpr int anchor_point_mask_correction_kind = anchor_point_correction_kind + 1;
    if (kind < 0 || kind > anchor_point_mask_correction_kind || !this->is_editable(owner)) {
      return -1;
    }
    const bool is_anchor_point = kind == anchor_point_kind;
    const bool is_anchor_point_layer = kind == anchor_point_layer_kind;
    const bool is_anchor_point_correction = kind == anchor_point_correction_kind;
    const bool is_anchor_point_mask_correction = kind == anchor_point_mask_correction_kind;
    const bool is_anchor_point_reader = is_anchor_point_layer || is_anchor_point_correction ||
                                        is_anchor_point_mask_correction;
    const bool is_material = kind == material_kind;
    const bool is_material_correction = kind == material_correction_kind;
    const bool is_mask_material = kind == mask_material_kind;
    const bool reads_material = is_material || is_material_correction || is_mask_material;
    /* The kind ranges are contiguous. Bounding each one is what keeps the Material and Anchor
     * Point kinds from falling into the last filter arrays, whose indices they are past. */
    const bool is_coverage_filter = kind >= coverage_first_kind && kind < material_kind;
    const bool is_mask_filter = kind >= mask_filter_first_kind && kind < material_kind;
    const bool is_adjustment = kind >= adjustment_first_kind && kind < mask_filter_first_kind;
    /* A filter correction hangs on a layer; an adjustment is a layer. */
    const bool is_filter = kind >= filter_first_kind && kind < adjustment_first_kind;
    Material &material = layers_owner(owner);
    MaterialPaintLayer *anchor = (ordinal < 0) ?
                                     nullptr :
                                     paint_description_row_for_ordinal(material, ordinal);
    if (ordinal >= 0 && anchor == nullptr) {
      if (is_anchor_point || is_anchor_point_reader) {
        BKE_report(CTX_wm_reports(&C),
                   RPT_WARNING,
                   "The selected row is no longer part of this material");
      }
      return -1;
    }
    PaintLayerAddParams params;
    if (is_filter) {
      params.filter = paint_filter_add_kinds[kind - filter_first_kind].filter;
    }
    if (is_adjustment) {
      params.filter = paint_filter_add_kinds[kind - adjustment_first_kind].filter;
    }
    if (is_mask_filter) {
      params.filter = is_coverage_filter ?
                          paint_coverage_add_kinds[kind - coverage_first_kind].filter :
                          paint_filter_add_kinds[kind - mask_filter_first_kind].filter;
    }
    if (reads_material) {
      /* The Add resolved the data-block it names before the call; a kind that needs one is never
       * called without it, but the call is also open to scripts. */
      if (args.source == nullptr || GS(args.source->name) != ID_MA) {
        BKE_report(CTX_wm_reports(&C), RPT_WARNING, "A Material layer needs a material to read");
        return -1;
      }
      params.source = id_cast<Material *>(args.source);
      /* Each Material row reads a material of its own, so its settings are its own. */
      params.bmain = CTX_data_main(&C);
    }
    params.kind = is_anchor_point              ? PaintLayerAddKind::AnchorPoint :
                  is_anchor_point_layer        ? PaintLayerAddKind::AnchorPointLayer :
                  is_anchor_point_correction   ? PaintLayerAddKind::AnchorPointCorrection :
                  is_anchor_point_mask_correction ?
                                                 PaintLayerAddKind::AnchorPointMaskCorrection :
                  is_material                  ? PaintLayerAddKind::Material :
                  is_material_correction ? PaintLayerAddKind::CorrectionMaterial :
                  is_mask_material       ? PaintLayerAddKind::MaskCorrectionMaterial :
                  is_mask_filter         ? PaintLayerAddKind::MaskFilter :
                  is_adjustment          ? PaintLayerAddKind::Adjustment :
                  is_filter              ? PaintLayerAddKind::Filter :
                  (kind == 1)            ? PaintLayerAddKind::Fill :
                  (kind == 2)            ? PaintLayerAddKind::Folder :
                  (kind == 3)            ? PaintLayerAddKind::Correction :
                  (kind == 4)            ? PaintLayerAddKind::CorrectionFill :
                  (kind == 5)            ? PaintLayerAddKind::MaskCorrection :
                  (kind == 6)            ? PaintLayerAddKind::MaskCorrectionFill :
                                           PaintLayerAddKind::Paint;
    if (is_anchor_point || is_anchor_point_reader) {
      /* The active item decides where the point lands: a layer or a content correction publishes
       * content, a mask item publishes the mask. Canonicalize the effect/mask item to its owner
       * row, so a layer inserted beside the active thing is always a stack row. */
      MaterialPaintLayer *active_item = nullptr;
      MaterialPaintLayer *active_effect = nullptr;
      MaterialPaintLayer *active_row = nullptr;
      if (ordinal >= 0) {
        if (anchor->role == MA_PAINT_LAYER_ROLE_EFFECT) {
          active_effect = anchor;
          active_row = BKE_paint_layers_correction_owner(material, *anchor);
        }
        else if (anchor->role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
          active_item = anchor;
          active_row = BKE_paint_layers_mask_item_owner(material, *anchor);
        }
        else {
          active_row = anchor;
        }
      }
      else {
        active_item = BKE_paint_layers_active_mask_item_get(material);
        MaterialPaintLayer *active = BKE_paint_layers_active_layer_get(material);
        if (active != nullptr && active->role == MA_PAINT_LAYER_ROLE_EFFECT) {
          active_effect = active;
          active_row = BKE_paint_layers_correction_owner(material, *active);
        }
        else {
          active_row = active;
        }
      }
      if (active_item == nullptr && active_effect == nullptr && active_row != nullptr) {
        /* The mask's base is no Outliner row: selecting its slot leaves the layer active and only
         * switches the global target to the mask. That is the user pointing at the mask, so the
         * point goes there, right after the base. */
        const Scene *scene = CTX_data_scene(&C);
        if (scene != nullptr && scene->toolsettings != nullptr &&
            scene->toolsettings->paint_mode.layer_target_mode == PAINT_LAYER_TARGET_MASK)
        {
          active_item = BKE_paint_layers_mask_base(*active_row);
        }
      }
      MaterialPaintLayer *point_owner = active_row;
      MaterialPaintLayer *insert_after = nullptr;
      PaintLayerAnchorPointList list = PaintLayerAnchorPointList::Effects;
      if (is_anchor_point_layer) {
        /* A reader layer is a row like a Material layer: above the active row, or on top. */
        list = PaintLayerAnchorPointList::Row;
        insert_after = active_row;
      }
      else if (is_anchor_point_mask_correction) {
        list = PaintLayerAnchorPointList::MaskStack;
        insert_after = active_item;
      }
      else if (is_anchor_point_correction) {
        list = PaintLayerAnchorPointList::Effects;
        insert_after = active_effect;
      }
      else {
        /* Marker: content under the active layer/effect, mask under the active mask item. */
        if (active_item != nullptr) {
          list = PaintLayerAnchorPointList::MaskStack;
          insert_after = active_item;
        }
        else if (active_effect != nullptr) {
          list = PaintLayerAnchorPointList::Effects;
          insert_after = active_effect;
        }
        else {
          list = PaintLayerAnchorPointList::Effects;
        }
      }
      if (point_owner == nullptr && list != PaintLayerAnchorPointList::Row) {
        BKE_report(CTX_wm_reports(&C),
                   RPT_WARNING,
                   is_anchor_point ? "Select a layer to add an Anchor Point to" :
                                     "Select a layer to add an Anchor Point Reader to");
        return -1;
      }
      anchor = point_owner;
      params.anchor_point_list = list;
      params.insert_after = insert_after;
    }
    const bool is_mask_correction = ELEM(params.kind,
                                         PaintLayerAddKind::MaskCorrection,
                                         PaintLayerAddKind::MaskCorrectionFill,
                                         PaintLayerAddKind::MaskCorrectionMaterial,
                                         PaintLayerAddKind::MaskFilter,
                                         PaintLayerAddKind::AnchorPointMaskCorrection);
    const bool is_correction = is_mask_correction || is_filter ||
                               ELEM(params.kind,
                                    PaintLayerAddKind::Correction,
                                    PaintLayerAddKind::CorrectionFill,
                                    PaintLayerAddKind::CorrectionMaterial,
                                    PaintLayerAddKind::AnchorPointCorrection);
    if (anchor != nullptr && !is_correction) {
      /* A correction is not a place a row can be added next to: the row it hangs on is. */
      if (MaterialPaintLayer *parent = BKE_paint_layers_correction_owner(material, *anchor)) {
        anchor = parent;
      }
    }
    if (is_correction && anchor == nullptr) {
      /* No row was named: the correction goes on the one being worked on. */
      anchor = BKE_paint_layers_active_layer_get(material);
      if (anchor == nullptr) {
        BKE_report(CTX_wm_reports(&C), RPT_WARNING, "Select a layer to add a correction to");
        return -1;
      }
    }

    params.anchor = anchor;
    /* An Add onto a folder means filling it: the policy turns this into Into unless the caller
     * aimed a line beside the folder. */
    params.place = (args.place == StackMovePlace::Below) ? PaintLayerPlace::Below :
                                                           PaintLayerPlace::Above;
    params.fill_folder = args.fill_folder;
    if (ELEM(params.kind,
             PaintLayerAddKind::Fill,
             PaintLayerAddKind::CorrectionFill,
             PaintLayerAddKind::MaskCorrectionFill) &&
        args.color != nullptr)
    {
      params.fill_color = args.color;
    }
    MaterialPaintLayer *created = BKE_paint_layers_add_with_policy(material, params);
    if (created == nullptr) {
      if (is_anchor_point || is_anchor_point_reader) {
        BKE_report(CTX_wm_reports(&C),
                   RPT_WARNING,
                   "Could not add the Anchor Point to this layer");
      }
      if (is_adjustment) {
        BKE_report(CTX_wm_reports(&C),
                   RPT_WARNING,
                   "An adjustment needs a Pass Through folder or the top of the stack");
      }
      if (reads_material) {
        BKE_report(CTX_wm_reports(&C),
                   RPT_WARNING,
                   "That material cannot be read here: it is this material, or it already reads it");
      }
      if (is_anchor_point || is_anchor_point_reader) {
        BKE_report(CTX_wm_reports(&C), RPT_WARNING, "Select a layer to add an Anchor Point to");
      }
      return -1;
    }
    /* A new layer is painted on its content, not on the mask the previous one was edited in; a mask
     * correction is the mask the user means to paint. */
    const bool anchor_in_mask = (is_anchor_point || is_anchor_point_reader) &&
                                params.anchor_point_list == PaintLayerAnchorPointList::MaskStack;
    layers_target_mode_set(C, (is_mask_correction || anchor_in_mask) ? PAINT_LAYER_TARGET_MASK :
                                                                       PAINT_LAYER_TARGET_CONTENT);
    if (is_anchor_point || is_anchor_point_reader) {
      /* The point or reader the user just added is the one they mean to work on. */
      BKE_paint_layers_active_set(material, created->marker);
      layers_texpaint_slots_refresh(C);
    }
    else if (is_correction) {
      /* The correction the user just added is the one they mean to work on: paint into, or for a
       * filter set the parameters and channels of. A filter has no map, so a stroke on it lands
       * nowhere. */
      BKE_paint_layers_active_set(material, created->marker);
      layers_texpaint_slots_refresh(C);
    }
    else if (params.kind == PaintLayerAddKind::Paint || is_adjustment || is_material) {
      /* The target resolves through the active row, and a plain Add leaves the previous one active
       * until the caller activates the new row, so the maps below would land on the wrong layer. */
      BKE_paint_layers_active_set(material, created->marker);
    }
    /* A row with no map makes the first stroke create it and regenerate the graph before it can
     * write, which loses that stroke. The active slot's map is made together with the row, so the
     * first stroke already lands in it; the other channels still get theirs when they are used. */
    if (ELEM(params.kind,
             PaintLayerAddKind::Paint,
             PaintLayerAddKind::Correction,
             PaintLayerAddKind::MaskCorrection))
    {
      if (BKE_paint_layers_active_layer_get(material) != nullptr) {
        const Scene *scene = CTX_data_scene(&C);
        const PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
        const int slot_channel = BKE_paint_layers_texpaint_slot_channel(material.paint_active_slot);
        PaintLayersTarget target;
        if (BKE_paint_layers_target_get(
                material,
                is_mask_correction || slot_channel < 0 ? int(PAINT_MATERIAL_CHANNEL_BASE_COLOR) :
                                                         slot_channel,
                BKE_paint_layers_target_mode(&paint_mode),
                target) &&
            BKE_paint_layers_target_image(target) == nullptr)
        {
          BKE_paint_layers_target_ensure_writable(
              *CTX_data_main(&C), target, paint_mode.new_channel_image_size);
          layers_texpaint_slots_refresh(C);
        }
      }
    }
    /* The other channels of a Paint layer are not made here: every paint entry point makes the
     * maps its stroke writes before the stroke starts (as a persistent side effect, not part of
     * that stroke's undo step), so a layer nothing is painted into holds one map instead of one
     * per channel of the set. A 4K map is 64 MiB of RAM and ~85 MiB of GPU memory each. */
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
    if (layer->role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
      /* The row it hangs on becomes the active one again, so the mode stays on its mask. */
      if (!BKE_paint_layers_mask_item_remove(material, layer)) {
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
    /* Drop the object mask records of the removed row (and its descendants) right away. */
    BKE_paint_layer_object_masks_cleanup_all(*CTX_data_main(&C));
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
    /* A list-drawing change only: the Outliner is the one view that shows it, and nothing
     * evaluated reads the tag. #stack_mutate sends the same notifier for the row edit; this keeps
     * the seam honest for a caller that reaches it another way. */
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
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

  int row_paste_correction(bContext &C,
                           const StackFocus & /*focus*/,
                           ID &owner,
                           const int source_ordinal,
                           const int target_ordinal) const override
  {
    Material &material = layers_owner(owner);
    Main *bmain = CTX_data_main(&C);
    const MaterialPaintLayer *source = paint_description_row_for_ordinal(material, source_ordinal);
    if (source == nullptr || !ELEM(source->role,
                                   MA_PAINT_LAYER_ROLE_EFFECT,
                                   MA_PAINT_LAYER_ROLE_MASK_ITEM))
    {
      return STACK_PASTE_NOT_APPLICABLE;
    }
    MaterialPaintLayer *target = paint_description_row_for_ordinal(material, target_ordinal);
    if (target != nullptr) {
      /* A correction or a mask item is no place to paste onto: the layer it hangs on is. */
      if (MaterialPaintLayer *parent = BKE_paint_layers_correction_owner(material, *target)) {
        target = parent;
      }
      else if (target->role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
        target = BKE_paint_layers_mask_item_owner(material, *target);
      }
    }
    if (bmain == nullptr || target == nullptr) {
      return -1;
    }
    MaterialPaintLayer *copy = BKE_paint_layers_correction_paste(*bmain, material, *source, target);
    if (copy == nullptr) {
      BKE_report(CTX_wm_reports(&C),
                 RPT_INFO,
                 "A mask correction needs a layer that has a mask to lay over");
      return -1;
    }
    /* What was pasted is what the user works on next, in the mode it belongs to. */
    BKE_paint_layers_active_set(material, copy->marker);
    layers_target_mode_set(C,
                           copy->role == MA_PAINT_LAYER_ROLE_MASK_ITEM ? PAINT_LAYER_TARGET_MASK :
                                                                         PAINT_LAYER_TARGET_CONTENT);
    layers_texpaint_slots_refresh(C);
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return paint_layers_ordinal_of(material, copy);
  }

  /** \} */

 private:
  /** Drop handler with the built-in actions and the FileHandler fallback. */
  std::unique_ptr<StackDropHandler> drop_handler_ = paint_layers_drop_handler_create();
};

std::unique_ptr<StackSource> stack_source_paint_layers_create()
{
  return std::make_unique<PaintLayersStackSource>();
}

/* -------------------------------------------------------------------- */
/** \name Assign Dropped Image to a Channel
 *
 * The popup an image drop opens: whichever channel the user picks is where the image lands -- on
 * the Fill layer it was dropped on, or on a new Fill layer beside the row it was aimed at (on top
 * when it named none).
 * \{ */

static int paint_channel_icon(const int channel)
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
  Material &material = layers_owner(*owner);
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
  /* An image cannot be assigned to a channel outside the material's set: the record would take
   * part nowhere. */
  if (!BKE_paint_layers_channel_in_set(material, channel)) {
    BKE_report(op->reports, RPT_ERROR, "The channel is not part of the material's channel set");
    return OPERATOR_CANCELLED;
  }

  MaterialPaintLayer *layer = nullptr;
  bool created = false;
  bool created_correction = false;
  if (RNA_string_length(op->ptr, "layer") > 0) {
    layer = stack_channel_image_assign_marker_layer(material, *op, "layer");
    if (layer == nullptr) {
      BKE_report(op->reports, RPT_ERROR, "The layer the image was dropped on is gone");
      return OPERATOR_CANCELLED;
    }
    /* The same BKE rule the drop gate applies: only a Fill row takes a map; a painted row's maps
     * belong to its brush. */
    switch (BKE_paint_layers_row_image_map_issue(*layer)) {
      case PaintLayerImageMapIssue::None:
        break;
      case PaintLayerImageMapIssue::PaintRowOwnsMaps:
        BKE_report(op->reports, RPT_ERROR, "The maps of a Paint layer cannot be replaced");
        return OPERATOR_CANCELLED;
      case PaintLayerImageMapIssue::TakesNoImage:
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
    /* The drop named a place with its line; an anchor folder does not swallow a row aimed beside
     * it, the way an Add button's Above would. */
    PaintLayerPlace place = (anchor != nullptr && RNA_boolean_get(op->ptr, "below")) ?
                                PaintLayerPlace::Below :
                                PaintLayerPlace::Above;
    if (anchor != nullptr) {
      if (MaterialPaintLayer *correction_owner = BKE_paint_layers_correction_owner(material,
                                                                                  *anchor))
      {
        /* Beside a correction the line means another correction of the row it hangs on, at that
         * place among them -- "between two corrections" included. A stack row cannot be linked
         * next to one, which is why this does not go through #BKE_paint_layers_add. */
        layer = BKE_paint_layers_correction_add(
            material, correction_owner, name, MA_PAINT_LAYER_SOURCE_CONSTANT);
        if (layer == nullptr || !BKE_paint_layers_edit_move(material, *layer, anchor, place)) {
          if (layer != nullptr) {
            BKE_paint_layers_correction_remove(material, layer);
          }
          BKE_report(op->reports, RPT_ERROR, "Could not add a correction for the image");
          return OPERATOR_CANCELLED;
        }
        created = true;
        created_correction = true;
      }
      else if (MaterialPaintLayer *mask_owner = BKE_paint_layers_mask_item_owner(material,
                                                                                 *anchor))
      {
        /* A mask item is no place for a stack row either: the row it belongs to is. */
        anchor = mask_owner;
      }
    }
    if (layer == nullptr) {
      /* Added bare, not through the Add policy: the policy gives a Fill the default channel set,
       * and the layer should take part in the one channel the user picked and no other. */
      layer = BKE_paint_layers_add(material, name, anchor, place);
      if (layer == nullptr) {
        BKE_report(op->reports, RPT_ERROR, "Could not add a layer for the image");
        return OPERATOR_CANCELLED;
      }
      layer->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
      created = true;
    }
  }

  if (BKE_paint_layers_channel_add(material, layer, channel) == nullptr ||
      !BKE_paint_layers_channel_set_image(material, layer, channel, image))
  {
    BKE_report(op->reports, RPT_ERROR, "The image could not be assigned to the channel");
    if (created) {
      /* A refused assignment must not read as a silent add. */
      if (created_correction) {
        BKE_paint_layers_correction_remove(material, layer);
      }
      else {
        BKE_paint_layers_remove(material, layer);
      }
    }
    return OPERATOR_CANCELLED;
  }
  if (created_correction && channel != PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    /* A Fill correction starts with a Base Color so a fresh one is visible; when the image landed
     * in another channel that flat colour must not cover the row's Base Color as well. */
    BKE_paint_layers_channel_remove(material, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
  /* What the Add wrapper does for a row it made: the cached rows go, the selection and the active
   * row move to the layer the image landed on, and the Outliner rebuilds. */
  const int ordinal = paint_layers_ordinal_of(material, layer);
  stack_row_edit_finished(*C, *space_outliner, material.id, ordinal, true);
  outliner_stack_row_activate(C, *space_outliner, ordinal);
  /* A correction is listed under its row's channels section: bring that section up so the
   * correction the drop just made is not there and unseen. */
  stack_row_reveal_attached(*space_outliner, ordinal);
  /* The edit helper above only tells the Outliner; the Properties editor and the previews show the
   * material. */
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material.id);
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_channel_image_assign(wmOperatorType *ot)
{
  ot->name = "Assign Image to Channel";
  ot->description =
      "Assign the dropped image to a channel of the layer, or add a new Fill layer or correction "
      "for it";
  ot->idname = "OUTLINER_OT_stack_layer_channel_image_assign";

  ot->invoke = stack_channel_image_assign_invoke;
  ot->exec = stack_channel_image_assign_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "channel", rna_enum_dummy_NULL_items, 0, "Channel", "Channel the image paints");
  RNA_def_enum_funcs(prop, stack_channel_image_assign_channel_itemf);
  /* A session UID is a uint32 read through an int, so the whole int range is its range: clamping
   * at INT32_MAX would fold every uid above it onto that value. */
  RNA_def_int(ot->srna,
              "image_uid",
              0,
              INT32_MIN,
              INT32_MAX,
              "Image",
              "",
              INT32_MIN,
              INT32_MAX);
  RNA_def_string(ot->srna,
                 "layer",
                 nullptr,
                 UUID_STRING_SIZE,
                 "Layer",
                 "Marker of the Fill layer the image was dropped on; empty adds a Fill layer of "
                 "its own");
  RNA_def_string(ot->srna,
                 "anchor",
                 nullptr,
                 UUID_STRING_SIZE,
                 "Anchor",
                 "Marker of the row or correction a new layer is placed beside; empty puts it on "
                 "top");
  RNA_def_boolean(ot->srna, "below", false, "Below", "Place a new layer below the anchor row");
}

/** \} */

}  // namespace blender::ed::outliner
