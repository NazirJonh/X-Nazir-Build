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
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_uuid_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_report.hh"

#include "BLI_assert.h"
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
    row.icon = ICON_IMAGE_RGB;

    /* The value column edits the row's opacity; a value left at its default reads as inherited
     * rather than deliberately set, so the control dims until the user moves it. */
    PointerRNA layer_ptr = RNA_pointer_create_discrete(
        &const_cast<Material &>(material).id,
        RNA_struct_find("MaterialPaintLayer"),
        const_cast<MaterialPaintLayer *>(&layer));
    row.value_ptr = layer_ptr;
    row.value_prop = "opacity";
    row.value_inherited = (layer.opacity == 1.0f);

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

    /* The channel maps are the layer's internals rather than rows of their own; the section stays
     * so the brush can switch to the content and later rows can attach to it. */
    StackContentSection channels;
    channels.identifier = "CHANNELS";
    channels.name = IFACE_("Channel Textures");
    row.content_sections.append(std::move(channels));

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

/** \} */

/* -------------------------------------------------------------------- */
/** \name The source
 * \{ */

class PaintLayersStackSource final : public StackSource, public StackEditor {
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
      /* The swatch and the columns show these, so a change to them has to reach the rows. */
      hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(layer.opacity));
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
    /* The blend mode column arrives with the blend modes; until then the row shows no mode. */
    layout.mode_width = 0.0f;
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
    /* Paint is the only kind that exists yet; Fill, Material and the corrections are appended
     * here as their features land, in the order #row_add dispatches by index. */
    r_kinds.append({"PAINT",
                    N_("Paint Layer"),
                    "A layer that shows nothing until it is painted on",
                    ICON_BRUSH_DATA});
  }

  int row_add(bContext &C,
              const StackFocus & /*focus*/,
              ID &owner,
              const int kind,
              const int ordinal,
              const StackAddArgs & /*args*/ = {}) const override
  {
    if (kind != 0 || !this->is_editable(owner)) {
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
    params.anchor = anchor;
    params.place = PaintLayerPlace::Above;
    MaterialPaintLayer *created = BKE_paint_layers_add_with_policy(material, params);
    if (created == nullptr) {
      return -1;
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
    if (layer == nullptr || !BKE_paint_layers_remove(material, layer)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

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
