/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The paint layers of a material, as a stack.
 *
 * The layers are not stored anywhere: they are the shape of the material's node graph, a chain of
 * Mix nodes over Image Textures, and #BKE_paint_material_layer_stack_from_material reads them back
 * out of it. That is deliberate -- the graph is what renders, so anything else would be a second
 * truth to keep in sync -- and it is why this source is a reader rather than an owner.
 *
 * Activating a row points #PaintModeSettings.channel_image_bindings at the layer's maps, which is
 * scene data and undoable. Which material last did that is not: it is a fact about this session's
 * navigation, kept here so that only the Outliner showing the owning material claims a row is
 * active. Two Outliners pointed at different materials must not both look like the paint target.
 */

#include <cstdio>

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_layer_edit.hh"
#include "BKE_report.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_set.hh"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "ED_image.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

namespace {

/**
 * The material whose layer the paint bindings currently point at.
 *
 * Session state, not file state: it exists so a row is highlighted only where it is true, and an
 * undo step that restores different bindings makes it a lie, which is what #undo_reset is for.
 */
const Material *g_bindings_owner = nullptr;

Material *paint_material_get(const StackFocus &focus)
{
  if (focus.object == nullptr) {
    return nullptr;
  }
  const short slot = focus.sub_index >= 0 ? short(focus.sub_index + 1) : focus.object->actcol;
  return BKE_object_material_get(focus.object, slot);
}

void paint_topology_count(const bNodeTree &tree,
                          Set<const bNodeTree *> &visited,
                          int64_t &r_nodes,
                          int64_t &r_links)
{
  if (!visited.add(&tree)) {
    return;
  }
  for (const bNode &node : tree.nodes) {
    r_nodes++;
    if (node.is_group() && node.id != nullptr) {
      paint_topology_count(*id_cast<const bNodeTree *>(node.id), visited, r_nodes, r_links);
    }
  }
  r_links += tree.links.count();
}

void paint_binding_set(MaterialPaintChannelImageBinding &binding, Image *image)
{
  if (binding.image == image) {
    return;
  }
  id_us_min(reinterpret_cast<ID *>(binding.image));
  binding.image = image;
  id_us_plus(reinterpret_cast<ID *>(binding.image));
  BKE_imageuser_default(&binding.iuser);
}

/**
 * The icon a channel's map is listed with.
 *
 * A layer expands into up to ten maps that are all Images; without this they are ten identical
 * rows differing only by name.
 */
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

/** An Image Editor already open in this screen, or null. */
ScrArea *outliner_image_area_find(const bContext &C)
{
  bScreen *screen = CTX_wm_screen(&const_cast<bContext &>(C));
  if (screen == nullptr) {
    return nullptr;
  }
  for (ScrArea &area : screen->areabase) {
    if (area.spacetype == SPACE_IMAGE) {
      return &area;
    }
  }
  return nullptr;
}

/** The layer's map for \a role, or null. Read back from the row rather than from the model. */
Image *paint_row_map_get(const StackRow &row, const int role)
{
  for (const StackSubRow &sub_row : row.sub_rows) {
    if (sub_row.role == role) {
      return reinterpret_cast<Image *>(sub_row.id);
    }
  }
  return nullptr;
}

/** Whether the row names a map at all. A group row names none: it is a folder, not a target. */
bool paint_row_has_map(const StackRow &row)
{
  for (const StackSubRow &sub_row : row.sub_rows) {
    if (sub_row.id != nullptr) {
      return true;
    }
  }
  return false;
}

class PaintMaterialStackSource final : public StackSource {
 public:
  eSpaceOutliner_StackSource type() const override
  {
    return SO_STACK_SRC_PAINT_MATERIAL;
  }

  StringRefNull ui_name() const override
  {
    return "Paint Layers";
  }

  bool object_has_stack(const StackReadContext & /*ctx*/, Object &object) const override
  {
    const Material *material = BKE_object_material_get(&object, object.actcol);
    return material != nullptr && BKE_paint_material_has_layer_stack(*material);
  }

  ID *owner_get(const StackReadContext & /*ctx*/, const StackFocus &focus) const override
  {
    Material *material = paint_material_get(focus);
    return material != nullptr ? &material->id : nullptr;
  }

  void sub_selection_names(const StackReadContext & /*ctx*/,
                           const StackFocus &focus,
                           Vector<std::string> &r_names) const override
  {
    if (focus.object == nullptr) {
      return;
    }
    /* One entry per material slot, empty slots included: the index is the slot, so skipping one
     * would shift every slot after it. */
    for (const int slot : IndexRange(focus.object->totcol)) {
      const Material *material = BKE_object_material_get(focus.object, short(slot + 1));
      r_names.append(material != nullptr ? material->id.name + 2 : "");
    }
  }

  uint64_t state_hash(const StackReadContext &ctx, const ID &owner) const override
  {
    const Material &material = reinterpret_cast<const Material &>(owner);
    int64_t nodes = 0;
    int64_t links = 0;
    if (material.nodetree != nullptr) {
      Set<const bNodeTree *> visited;
      paint_topology_count(*material.nodetree, visited, nodes, links);
    }
    /* Maps are found by tag as well as by link, so a tagged image appearing or disappearing
     * changes the rows without changing the graph. */
    int64_t tagged_images = 0;
    if (ctx.bmain != nullptr) {
      for (const Image &image : ctx.bmain->images) {
        if (!BLI_uuid_is_nil(image.paint_layer_id)) {
          tagged_images++;
        }
      }
    }
    return uint64_t(nodes) * 2654435761u ^ uint64_t(links) * 40503u ^ uint64_t(tagged_images);
  }

  bool rows_build(const StackReadContext &ctx,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  Vector<StackRow> &r_rows) const override
  {
    if (ctx.bmain == nullptr) {
      return false;
    }
    const Material &material = reinterpret_cast<const Material &>(owner);
    Vector<PaintMaterialLayerStackEntry> entries;
    if (!BKE_paint_material_layer_stack_from_material(*ctx.bmain, material, entries)) {
      return false;
    }
    for (PaintMaterialLayerStackEntry &entry : entries) {
      if (entry.ordinal > STACK_ROW_ORDINAL_MAX) {
        break;
      }
      StackRow row;
      row.ordinal = entry.ordinal;
      row.depth = entry.depth;
      row.stable_id = entry.marker;
      /* Filled in below: the model lists a group after the layers it holds, so the enclosing row
       * is not in `r_rows` yet. */
      row.parent_ordinal = -1;
      row.is_group = entry.is_group;
      row.is_bare_base = entry.is_bare_base;
      row.enabled = entry.enabled;
      row.supported = entry.supported;
      row.unsupported_reason = entry.unsupported_reason;
      row.name = std::move(entry.name);
      row.name_buffer = entry.label;
      row.icon = entry.is_group ? ICON_FILE_FOLDER :
                 entry.has_mask ? ICON_MOD_MASK :
                                  ICON_IMAGE_RGB;
      if (entry.factor_prop) {
        row.value_ptr = *entry.factor_prop;
        /* #RNA_PaintMaterialLayerOpacity's own "value", always 0-100%; see
         * #layer_model_entry_from_node. */
        row.value_prop = "value";
      }
      if (entry.blend_prop) {
        row.mode_ptr = *entry.blend_prop;
        row.mode_prop = "blend_type";
      }
      for (const auto item : entry.channel_images.items()) {
        if (item.value == nullptr || item.key >= STACK_ROW_SUB_ROW_STRIDE) {
          continue;
        }
        StackSubRow sub_row;
        sub_row.role = item.key;
        sub_row.name = item.value->id.name + 2;
        sub_row.id = &item.value->id;
        sub_row.icon = paint_channel_icon(item.key);
        row.sub_rows.append(std::move(sub_row));
      }
      r_rows.append(std::move(row));
    }
    /* A group is listed after the rows it holds -- that is the order the reader walks the graph in
     * -- so the enclosing row of a nested one is the first row *after* it that sits one level up.
     * Resolved here, once every row exists. */
    for (const int64_t index : r_rows.index_range()) {
      StackRow &row = r_rows[index];
      if (row.depth == 0) {
        continue;
      }
      for (int64_t next = index + 1; next < r_rows.size(); next++) {
        if (r_rows[next].depth == row.depth - 1) {
          row.parent_ordinal = r_rows[next].ordinal;
          break;
        }
      }
    }
    return !r_rows.is_empty();
  }

  bool is_editable(const ID &owner) const override
  {
    const Material &material = reinterpret_cast<const Material &>(owner);
    return ID_IS_EDITABLE(&material.id) && !ID_IS_OVERRIDE_LIBRARY(&material.id) &&
           material.nodetree != nullptr && ID_IS_EDITABLE(&material.nodetree->id) &&
           !ID_IS_OVERRIDE_LIBRARY(material.nodetree);
  }

  StackColumnLayout column_layout() const override
  {
    return {2, 2};
  }

  bool row_activate(bContext &C,
                    const StackFocus & /*focus*/,
                    ID &owner,
                    const int /*ordinal*/,
                    const StackRow &row) const override
  {
    Material &material = reinterpret_cast<Material &>(owner);
    /* The UI greys these out already, but a shortcut or a Python call reaches the operator
     * directly. */
    if (!this->is_editable(owner)) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Paint layer graph is not editable");
      return false;
    }
    if (!row.supported) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Unsupported paint layer cannot be activated");
      return false;
    }
    Scene *scene = CTX_data_scene(&C);
    if (scene == nullptr || scene->toolsettings == nullptr) {
      return false;
    }
    for (const StackSubRow &sub_row : row.sub_rows) {
      if (sub_row.id != nullptr && !ID_IS_EDITABLE(sub_row.id)) {
        BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Paint target image is not editable");
        return false;
      }
    }

    /* A row that names no map -- a group -- is not something the brush can write into, and
     * clearing the bindings for it would silently take the paint target away from the user. */
    if (!paint_row_has_map(row)) {
      return true;
    }

    PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
    for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
      paint_binding_set(paint_mode.channel_image_bindings[channel],
                        paint_row_map_get(row, channel));
    }
    g_bindings_owner = &material;
    WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
    return true;
  }

  bool row_is_active(const StackReadContext &ctx,
                     const StackFocus & /*focus*/,
                     const ID &owner,
                     const StackRow &row) const override
  {
    if (g_bindings_owner != reinterpret_cast<const Material *>(&owner) || !row.supported) {
      return false;
    }
    /* Without this, every map-less row -- every group -- would answer "yes" the moment no image is
     * bound, because it would compare its own nothing against the bindings' nothing. */
    if (!paint_row_has_map(row)) {
      return false;
    }
    if (ctx.scene == nullptr || ctx.scene->toolsettings == nullptr) {
      return false;
    }
    const PaintModeSettings &paint_mode = ctx.scene->toolsettings->paint_mode;
    for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
      if (paint_mode.channel_image_bindings[channel].image != paint_row_map_get(row, channel)) {
        return false;
      }
    }
    return true;
  }

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
    Material &material = reinterpret_cast<Material &>(owner);
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    if (!BKE_paint_material_layer_reorder(
            *CTX_data_main(&C), material, from_ordinal, to_ordinal, &error))
    {
      BKE_report(CTX_wm_reports(&C),
                 RPT_ERROR,
                 RPT_(BKE_paint_material_layer_edit_error_message(error)));
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
    Material &material = reinterpret_cast<Material &>(owner);
    /* The seam speaks of rows, this file speaks of layers; the two vocabularies meet here. */
    PaintMaterialLayerMovePlace layer_place = PaintMaterialLayerMovePlace::Above;
    switch (place) {
      case StackMovePlace::Above:
        layer_place = PaintMaterialLayerMovePlace::Above;
        break;
      case StackMovePlace::Below:
        layer_place = PaintMaterialLayerMovePlace::Below;
        break;
      case StackMovePlace::Into:
        layer_place = PaintMaterialLayerMovePlace::Into;
        break;
    }
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    if (!BKE_paint_material_layer_move(*CTX_data_main(&C),
                                       material,
                                       from_ordinal,
                                       anchor_ordinal,
                                       layer_place,
                                       r_ordinal,
                                       &error))
    {
      BKE_report(CTX_wm_reports(&C),
                 RPT_ERROR,
                 RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool can_add(const ID &owner) const override
  {
    /* A material with no stack yet can still be given one, so this asks nothing about the chain. */
    return this->is_editable(owner);
  }

  int row_add(bContext &C,
              const StackFocus & /*focus*/,
              ID &owner,
              const StackAddKind kind,
              const int ordinal) const override
  {
    Material &material = reinterpret_cast<Material &>(owner);
    PaintMaterialLayerAddParams params;
    params.type = (kind == StackAddKind::Fill) ? PaintMaterialLayerAddType::Fill :
                                                 PaintMaterialLayerAddType::Image;
    params.ordinal = ordinal;
    const Scene *scene = CTX_data_scene(&C);
    if (scene != nullptr && scene->toolsettings != nullptr) {
      /* The same size the first brush stroke would have created this material's maps at. */
      params.image_size = scene->toolsettings->paint_mode.new_channel_image_size;
    }

    int new_ordinal = -1;
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    if (!BKE_paint_material_layer_add(
            *CTX_data_main(&C), material, params, &new_ordinal, &error))
    {
      BKE_report(
          CTX_wm_reports(&C), RPT_ERROR, RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return new_ordinal;
  }

  bool can_set_enabled(const ID &owner) const override
  {
    /* Turning a layer off mutes its Mix nodes, so it needs the same permissions as any other
     * graph edit. */
    return this->is_editable(owner);
  }

  bool row_set_enabled(bContext &C,
                       const StackFocus & /*focus*/,
                       ID &owner,
                       const int ordinal,
                       const bool enable) const override
  {
    Material &material = reinterpret_cast<Material &>(owner);
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    if (!BKE_paint_material_layer_set_enabled(
            *CTX_data_main(&C), material, ordinal, enable, &error))
    {
      BKE_report(
          CTX_wm_reports(&C), RPT_ERROR, RPT_(BKE_paint_material_layer_edit_error_message(error)));
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
    Material &material = reinterpret_cast<Material &>(owner);
    int new_ordinal = -1;
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    if (!BKE_paint_material_layer_duplicate(
            *CTX_data_main(&C), material, ordinal, &new_ordinal, &error))
    {
      BKE_report(
          CTX_wm_reports(&C), RPT_ERROR, RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return new_ordinal;
  }

  bool row_rename(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal,
                  const StringRefNull name) const override
  {
    Material &material = reinterpret_cast<Material &>(owner);
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    if (!BKE_paint_material_layer_rename(
            *CTX_data_main(&C), material, ordinal, name.c_str(), &error))
    {
      BKE_report(
          CTX_wm_reports(&C), RPT_ERROR, RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool can_mask(const ID &owner) const override
  {
    return this->is_editable(owner);
  }

  bool row_mask_set(bContext &C,
                    const StackFocus & /*focus*/,
                    ID &owner,
                    const int ordinal,
                    const bool add) const override
  {
    Material &material = reinterpret_cast<Material &>(owner);
    Main &bmain = *CTX_data_main(&C);
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    bool ok = false;
    if (add) {
      int image_size = 1024;
      const Scene *scene = CTX_data_scene(&C);
      if (scene != nullptr && scene->toolsettings != nullptr) {
        image_size = scene->toolsettings->paint_mode.new_channel_image_size;
      }
      ok = BKE_paint_material_layer_mask_add(bmain, material, ordinal, image_size, &error);
    }
    else {
      ok = BKE_paint_material_layer_mask_remove(bmain, material, ordinal, &error);
    }
    if (!ok) {
      BKE_report(
          CTX_wm_reports(&C), RPT_ERROR, RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  int rows_group(bContext &C,
                 const StackFocus & /*focus*/,
                 ID &owner,
                 const int from_ordinal,
                 const int to_ordinal) const override
  {
    Material &material = reinterpret_cast<Material &>(owner);
    int group_ordinal = -1;
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    if (!BKE_paint_material_layer_group_make(
            *CTX_data_main(&C), material, from_ordinal, to_ordinal, &group_ordinal, &error))
    {
      BKE_report(
          CTX_wm_reports(&C), RPT_ERROR, RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return group_ordinal;
  }

  int group_add(bContext &C,
                const StackFocus & /*focus*/,
                ID &owner,
                const int ordinal) const override
  {
    Material &material = reinterpret_cast<Material &>(owner);
    int group_ordinal = -1;
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    if (!BKE_paint_material_layer_group_add(
            *CTX_data_main(&C), material, ordinal, &group_ordinal, &error))
    {
      BKE_report(
          CTX_wm_reports(&C), RPT_ERROR, RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return group_ordinal;
  }

  int row_ungroup(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal) const override
  {
    Material &material = reinterpret_cast<Material &>(owner);
    int layer_num = -1;
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    if (!BKE_paint_material_layer_group_ungroup(
            *CTX_data_main(&C), material, ordinal, &layer_num, &error))
    {
      BKE_report(
          CTX_wm_reports(&C), RPT_ERROR, RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return layer_num;
  }

  bool can_remove(const ID &owner) const override
  {
    return this->is_editable(owner);
  }

  bool row_remove(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal) const override
  {
    Material &material = reinterpret_cast<Material &>(owner);
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    if (!BKE_paint_material_layer_remove(*CTX_data_main(&C), material, ordinal, &error)) {
      BKE_report(CTX_wm_reports(&C),
                 RPT_ERROR,
                 RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return false;
    }
    /* The removed layer may well have been the paint target. */
    g_bindings_owner = nullptr;
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool sub_row_activate(bContext &C,
                        const StackFocus & /*focus*/,
                        ID & /*owner*/,
                        const StackRow & /*row*/,
                        const StackSubRow &sub_row) const override
  {
    if (sub_row.id == nullptr) {
      return false;
    }
    /* An Image Editor the user already has open is where they expect the map to appear. Only when
     * there is none does this take over the Outliner's own area, which is a destructive enough
     * change that guessing it twice would be wrong. */
    ScrArea *area = outliner_image_area_find(C);
    if (area == nullptr) {
      area = CTX_wm_area(&C);
      if (area == nullptr) {
        return false;
      }
      ED_area_newspace(&C, area, SPACE_IMAGE, false);
    }
    SpaceImage *space_image = static_cast<SpaceImage *>(area->spacedata.first);
    ED_space_image_set(CTX_data_main(&C), space_image, reinterpret_cast<Image *>(sub_row.id), false);
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_IMAGE, space_image);
    return true;
  }

  bool target_clear(bContext &C) const override
  {
    Scene *scene = CTX_data_scene(&C);
    if (scene == nullptr || scene->toolsettings == nullptr) {
      return false;
    }
    for (MaterialPaintChannelImageBinding &binding :
         scene->toolsettings->paint_mode.channel_image_bindings)
    {
      paint_binding_set(binding, nullptr);
    }
    g_bindings_owner = nullptr;
    WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
    return true;
  }

  void undo_reset() const override
  {
    /* Session UIDs survive undo, addresses do not, and the bindings this pointer claims to own may
     * have been restored to something else entirely. */
    g_bindings_owner = nullptr;
  }

  bool normalize_for_read(bContext &C, ID &owner) const override
  {
    if (!this->is_editable(owner)) {
      return false;
    }
    Material &material = reinterpret_cast<Material &>(owner);
    if (!BKE_paint_material_layer_bottom_normalize(*CTX_data_main(&C), material)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }
};

}  // namespace

std::unique_ptr<StackSource> stack_source_paint_material_create()
{
  return std::make_unique<PaintMaterialStackSource>();
}

}  // namespace blender::ed::outliner
