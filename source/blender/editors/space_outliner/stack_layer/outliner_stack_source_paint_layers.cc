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

#include <functional>
#include <string>

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
  if (BLI_uuid_equal(material.active_layer_marker, layer.marker)) {
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

void paint_stack_rows_from_description(const Material &material,
                                       const int channel,
                                       Vector<StackRow> &r_rows)
{
  /* The value/mode columns address the selected channel; until the per (row, channel) settings
   * exist they fall back to the row's own opacity, so the channel is not read here yet. */
  static_cast<void>(channel);

  int ordinal = 0;
  for (const MaterialPaintLayer &layer : material.paint_layers) {
    if (ordinal > STACK_ROW_ORDINAL_MAX) {
      StackRow overflow_row;
      overflow_row.ordinal = STACK_ROW_ORDINAL_MAX + 1;
      overflow_row.supported = false;
      overflow_row.unsupported_reason = "Stack is too large to display";
      overflow_row.icon = ICON_IMAGE_RGB;
      r_rows.append(std::move(overflow_row));
      break;
    }
    StackRow row;
    row.ordinal = int16_t(ordinal++);
    row.depth = 0;
    row.parent_ordinal = -1;
    row.stable_id = layer.marker;
    row.enabled = (layer.flag & MA_PAINT_LAYER_ENABLED) != 0;
    row.supported = true;
    row.name = layer.name[0] != '\0' ? layer.name : "Layer";
    row.name_buffer = const_cast<char *>(layer.name);
    row.icon = (layer.source == MA_PAINT_LAYER_SOURCE_CONSTANT) ? ICON_GP_DRAW_FILL :
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

    if (layer.source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
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

    /* The channel maps are the layer's internals rather than rows of their own; the section stays
     * so the brush can switch to the content and later rows can attach to it. */
    StackContentSection channels;
    channels.identifier = "CHANNELS";
    channels.name = IFACE_("Channel Textures");
    row.content_sections.append(std::move(channels));

    /* The mask: a slot of its own, whose click switches the brush to it, and the section that slot
     * reveals. Its switch rides on the row, so a switched-off mask is kept and listed. */
    if (const MaterialPaintLayer *mask = BKE_paint_layers_mask_base(layer)) {
      row.preview_slots.append(paint_mask_slot_build(*mask));
      row.aux_enabled = (mask->flag & MA_PAINT_LAYER_ENABLED) != 0;
      StackContentSection mask_section;
      mask_section.identifier = "MASK";
      mask_section.name = IFACE_("Mask Content");
      row.content_sections.append(std::move(mask_section));
    }

    r_rows.append(std::move(row));
  }
}

MaterialPaintLayer *paint_description_row_for_ordinal(Material &material, const int ordinal)
{
  if (ordinal < 0) {
    return nullptr;
  }
  int index = 0;
  for (MaterialPaintLayer &layer : material.paint_layers) {
    if (index == ordinal) {
      return &layer;
    }
    index++;
  }
  return nullptr;
}

int paint_layers_ordinal_of(const Material &material, const MaterialPaintLayer *target)
{
  if (target == nullptr) {
    return -1;
  }
  int index = 0;
  for (const MaterialPaintLayer &layer : material.paint_layers) {
    if (&layer == target) {
      return index;
    }
    index++;
  }
  return -1;
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
    for (const MaterialPaintLayer &layer : material.paint_layers) {
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
    }
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
    /* The kinds whose features exist: Paint and Fill. Material and the corrections are appended
     * here as they land, in the order #row_add dispatches by index. */
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
  }

  int row_add(bContext &C,
              const StackFocus & /*focus*/,
              ID &owner,
              const int kind,
              const int ordinal,
              const StackAddArgs &args = {}) const override
  {
    if ((kind != 0 && kind != 1) || !this->is_editable(owner)) {
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
    params.kind = (kind == 1) ? PaintLayerAddKind::Fill : PaintLayerAddKind::Paint;
    params.anchor = anchor;
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
    layers_mask_edit_end_if_active(C, material, *layer);
    if (!BKE_paint_layers_remove(material, layer)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  /* ---------------------------------------------------------------- */
  /** \name StackGroupingEditor
   *
   * Only the mask half exists: folders, merging and colour tags arrive with their features, so
   * their verbs keep the defaults that refuse.
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
    MaterialPaintLayer *anchor = (anchor_ordinal < 0) ?
                                     nullptr :
                                     paint_description_row_for_ordinal(material, anchor_ordinal);
    if (from == nullptr || (anchor_ordinal >= 0 && anchor == nullptr)) {
      return false;
    }
    /* Folders do not exist yet, so a drop "into" a row lands beside it as "above". */
    const PaintLayerPlace layer_place = (place == StackMovePlace::Below) ? PaintLayerPlace::Below :
                                                                           PaintLayerPlace::Above;
    if (!BKE_paint_layers_move(material, from, anchor, layer_place)) {
      return false;
    }
    if (r_ordinal != nullptr) {
      *r_ordinal = paint_layers_ordinal_of(material, from);
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  /** \} */
};

std::unique_ptr<StackSource> stack_source_paint_layers_create()
{
  return std::make_unique<PaintLayersStackSource>();
}

}  // namespace blender::ed::outliner
