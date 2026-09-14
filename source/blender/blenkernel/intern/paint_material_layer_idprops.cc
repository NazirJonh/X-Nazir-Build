/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See `paint_material_layer_idprops.hh`.
 */

#include "paint_material_layer_idprops.hh"

#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"

#include "BLI_assert.h"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include <fmt/format.h>

#include <cstring>
#include <string>

namespace blender::bke::paint_layer {

IDProperty *node_properties_ensure(bNode &node)
{
  if (node.prop == nullptr) {
    IDPropertyTemplate val = {0};
    node.prop = IDP_New(IDP_GROUP, &val, "RNA");
  }
  return node.prop;
}

bUUID marker_get(const bNode &node)
{
  if (node.prop == nullptr) {
    return BLI_uuid_nil();
  }
  const IDProperty *marker = IDP_GetPropertyTypeFromGroup(
      node.prop, LAYER_MARKER_PROP, IDP_STRING);
  if (marker == nullptr) {
    return BLI_uuid_nil();
  }
  bUUID uuid = BLI_uuid_nil();
  if (!BLI_uuid_parse_string(&uuid, IDP_string_get(marker))) {
    return BLI_uuid_nil();
  }
  return uuid;
}

void marker_set(bNode &node, const bUUID &layer_id)
{
  char formatted[UUID_STRING_SIZE];
  BLI_uuid_format(formatted, layer_id);

  IDProperty *properties = node_properties_ensure(node);
  IDProperty *marker = IDP_GetPropertyTypeFromGroup(properties, LAYER_MARKER_PROP, IDP_STRING);
  if (marker != nullptr) {
    IDP_AssignString(marker, formatted);
    return;
  }
  IDP_AddToGroup(properties, IDP_NewString(formatted, LAYER_MARKER_PROP));
}

int color_tag_get(const bNode &node)
{
  if (node.prop == nullptr) {
    return -1;
  }
  const IDProperty *color_tag = IDP_GetPropertyTypeFromGroup(
      node.prop, LAYER_COLOR_TAG_PROP, IDP_INT);
  if (color_tag == nullptr) {
    return -1;
  }
  return IDP_int_get(color_tag);
}

void color_tag_set(bNode &node, const int color_tag)
{
  IDProperty *properties = node_properties_ensure(node);
  IDProperty *tag_prop = IDP_GetPropertyTypeFromGroup(properties, LAYER_COLOR_TAG_PROP, IDP_INT);
  if (tag_prop != nullptr) {
    IDP_int_set(tag_prop, color_tag);
    return;
  }
  IDP_AddToGroup(properties, IDP_NewInt(color_tag, LAYER_COLOR_TAG_PROP));
}

PaintMaterialLayerKind kind_get(const bNode &node)
{
  if (node.prop == nullptr) {
    return PaintMaterialLayerKind::Paint;
  }
  const IDProperty *kind = IDP_GetPropertyTypeFromGroup(node.prop, LAYER_KIND_PROP, IDP_INT);
  if (kind == nullptr) {
    return PaintMaterialLayerKind::Paint;
  }
  const int value = IDP_int_get(kind);
  if (value < 0 || value > int8_t(PaintMaterialLayerKind::Correction)) {
    /* A value this build does not know -- written by a newer one, or by hand -- reads as Paint
     * rather than as a number outside the enum. */
    return PaintMaterialLayerKind::Paint;
  }
  return static_cast<PaintMaterialLayerKind>(value);
}

void kind_set(bNode &node, const PaintMaterialLayerKind kind)
{
  IDProperty *properties = node_properties_ensure(node);
  IDProperty *kind_prop = IDP_GetPropertyTypeFromGroup(properties, LAYER_KIND_PROP, IDP_INT);
  if (kind_prop != nullptr) {
    IDP_int_set(kind_prop, int8_t(kind));
    return;
  }
  IDP_AddToGroup(properties, IDP_NewInt(int8_t(kind), LAYER_KIND_PROP));
}

PaintMaterialCorrectionSection correction_section_get(const bNode &node)
{
  if (node.prop == nullptr) {
    return PaintMaterialCorrectionSection::Content;
  }
  const IDProperty *section = IDP_GetPropertyTypeFromGroup(
      node.prop, CORRECTION_SECTION_PROP, IDP_INT);
  if (section == nullptr) {
    return PaintMaterialCorrectionSection::Content;
  }
  return static_cast<PaintMaterialCorrectionSection>(IDP_int_get(section));
}

void correction_section_set(bNode &node, const PaintMaterialCorrectionSection section)
{
  IDProperty *properties = node_properties_ensure(node);
  IDProperty *section_prop = IDP_GetPropertyTypeFromGroup(
      properties, CORRECTION_SECTION_PROP, IDP_INT);
  if (section_prop != nullptr) {
    IDP_int_set(section_prop, int8_t(section));
    return;
  }
  IDP_AddToGroup(properties, IDP_NewInt(int8_t(section), CORRECTION_SECTION_PROP));
}

PaintMaterialCorrectionEffect correction_effect_get(const bNode &node)
{
  if (node.prop == nullptr) {
    return PaintMaterialCorrectionEffect::Paint;
  }
  const IDProperty *effect = IDP_GetPropertyTypeFromGroup(
      node.prop, CORRECTION_EFFECT_PROP, IDP_INT);
  if (effect == nullptr) {
    return PaintMaterialCorrectionEffect::Paint;
  }
  return static_cast<PaintMaterialCorrectionEffect>(IDP_int_get(effect));
}

void correction_effect_set(bNode &node, const PaintMaterialCorrectionEffect effect)
{
  IDProperty *properties = node_properties_ensure(node);
  IDProperty *effect_prop = IDP_GetPropertyTypeFromGroup(
      properties, CORRECTION_EFFECT_PROP, IDP_INT);
  if (effect_prop != nullptr) {
    IDP_int_set(effect_prop, int8_t(effect));
    return;
  }
  IDP_AddToGroup(properties, IDP_NewInt(int8_t(effect), CORRECTION_EFFECT_PROP));
}

bool node_is_correction(const bNode &node)
{
  return kind_get(node) == PaintMaterialLayerKind::Correction;
}

bool fill_color_get(const bNode &node, float r_color[4])
{
  if (node.prop == nullptr) {
    return false;
  }
  const IDProperty *fill = IDP_GetPropertyTypeFromGroup(
      node.prop, LAYER_FILL_COLOR_PROP, IDP_ARRAY);
  if (fill == nullptr || fill->subtype != IDP_FLOAT || fill->len != 4) {
    return false;
  }
  copy_v4_v4(r_color,
             static_cast<const float *>(IDP_array_voidp_get(const_cast<IDProperty *>(fill))));
  return true;
}

void fill_color_set(bNode &node, const float color[4])
{
  IDProperty *properties = node_properties_ensure(node);
  IDProperty *fill = IDP_GetPropertyTypeFromGroup(properties, LAYER_FILL_COLOR_PROP, IDP_ARRAY);
  if (fill != nullptr && fill->subtype == IDP_FLOAT && fill->len == 4) {
    copy_v4_v4(static_cast<float *>(IDP_array_voidp_get(fill)), color);
    return;
  }
  if (fill != nullptr) {
    /* A stale array of the wrong shape is replaced rather than reused. */
    IDP_RemoveFromGroup(properties, fill);
    IDP_FreeProperty(fill);
  }
  IDP_AddToGroup(properties,
                 bke::idprop::create(LAYER_FILL_COLOR_PROP, Span<float>(color, 4)).release());
}

std::string channel_value_idprop_name(const int channel)
{
  /* A key holds 64 characters; the prefix leaves room for any channel number. */
  return fmt::format("{}{}", LAYER_CHANNEL_VALUE_PROP_PREFIX, channel);
}

bool channel_value_get(const bNode &node, const int channel, float r_color[4])
{
  if (node.prop == nullptr) {
    return false;
  }
  const std::string name = channel_value_idprop_name(channel);
  const IDProperty *value = IDP_GetPropertyTypeFromGroup(node.prop, name.c_str(), IDP_ARRAY);
  if (value == nullptr || value->subtype != IDP_FLOAT || value->len != 4) {
    return false;
  }
  copy_v4_v4(r_color,
             static_cast<const float *>(IDP_array_voidp_get(const_cast<IDProperty *>(value))));
  return true;
}

void channel_value_set(bNode &node, const int channel, const float color[4])
{
  IDProperty *properties = node_properties_ensure(node);
  const std::string name = channel_value_idprop_name(channel);
  IDProperty *value = IDP_GetPropertyTypeFromGroup(properties, name.c_str(), IDP_ARRAY);
  if (value != nullptr && value->subtype == IDP_FLOAT && value->len == 4) {
    copy_v4_v4(static_cast<float *>(IDP_array_voidp_get(value)), color);
    return;
  }
  if (value != nullptr) {
    /* A stale array of the wrong shape is replaced rather than reused. */
    IDP_RemoveFromGroup(properties, value);
    IDP_FreeProperty(value);
  }
  IDP_AddToGroup(properties, bke::idprop::create(name, Span<float>(color, 4)).release());
}

std::string channel_image_assigned_idprop_name(const int channel)
{
  /* A key holds 64 characters; the prefix leaves room for any channel number. */
  return fmt::format("{}{}", LAYER_CHANNEL_IMAGE_ASSIGNED_PROP_PREFIX, channel);
}

bool channel_image_assigned_get(const bNode &node, const int channel)
{
  if (node.prop == nullptr) {
    return false;
  }
  const std::string name = channel_image_assigned_idprop_name(channel);
  const IDProperty *assigned = IDP_GetPropertyTypeFromGroup(node.prop, name.c_str(), IDP_INT);
  return (assigned != nullptr) && (IDP_int_get(assigned) != 0);
}

void channel_image_assigned_set(bNode &node, const int channel, const bool assigned)
{
  IDProperty *properties = node_properties_ensure(node);
  const std::string name = channel_image_assigned_idprop_name(channel);
  IDProperty *record = IDP_GetPropertyTypeFromGroup(properties, name.c_str(), IDP_INT);
  if (!assigned) {
    /* Absence is the not-assigned state, so a clear removes the key instead of storing a zero:
     * a channel that never showed an image reads the same as one that was unlinked. */
    if (record != nullptr) {
      IDP_RemoveFromGroup(properties, record);
      IDP_FreeProperty(record);
    }
    return;
  }
  if (record != nullptr) {
    IDP_int_set(record, 1);
    return;
  }
  IDP_AddToGroup(properties, IDP_NewInt(1, name.c_str()));
}

void layer_group_tree_marker_set(bNodeTree &group)
{
  IDProperty *properties = IDP_EnsureProperties(&group.id);
  IDPropertyTemplate value = {0};
  value.string.str = const_cast<char *>(LAYER_GROUP_MARKER_VALUE);
  value.string.len = int(strlen(LAYER_GROUP_MARKER_VALUE)) + 1;
  value.string.subtype = IDP_STRING_SUB_UTF8;
  IDP_AddToGroup(properties, IDP_New(IDP_STRING, &value, NODE_GROUP_MARKER_PROP));
}

void normal_combine_tree_marker_set(bNodeTree &group)
{
  IDProperty *properties = IDP_EnsureProperties(&group.id);
  IDPropertyTemplate value = {0};
  value.string.str = const_cast<char *>(NORMAL_COMBINE_MARKER_VALUE);
  value.string.len = int(strlen(NORMAL_COMBINE_MARKER_VALUE)) + 1;
  value.string.subtype = IDP_STRING_SUB_UTF8;
  IDP_AddToGroup(properties, IDP_New(IDP_STRING, &value, NODE_GROUP_MARKER_PROP));
}

const char *node_tree_group_marker_get(const ID &tree_id)
{
  const IDProperty *properties = IDP_GetProperties(const_cast<ID *>(&tree_id));
  if (properties == nullptr) {
    return nullptr;
  }
  const IDProperty *marker = IDP_GetPropertyTypeFromGroup(
      properties, NODE_GROUP_MARKER_PROP, IDP_STRING);
  return (marker != nullptr) ? IDP_string_get(marker) : nullptr;
}

Material *group_material_get(const ID &group_tree_id)
{
  const IDProperty *properties = IDP_GetProperties(const_cast<ID *>(&group_tree_id));
  if (properties == nullptr) {
    return nullptr;
  }
  const IDProperty *reference = IDP_GetPropertyTypeFromGroup(
      properties, LAYER_GROUP_MATERIAL_PROP, IDP_ID);
  ID *id = (reference == nullptr) ? nullptr : IDP_ID_get(const_cast<IDProperty *>(reference));
  return (id != nullptr && GS(id->name) == ID_MA) ? id_cast<Material *>(id) : nullptr;
}

void group_material_set(ID &group_tree_id, Material *material)
{
  IDProperty *properties = IDP_EnsureProperties(&group_tree_id);
  IDProperty *reference = IDP_GetPropertyTypeFromGroup(
      properties, LAYER_GROUP_MATERIAL_PROP, IDP_ID);
  if (reference == nullptr) {
    if (material == nullptr) {
      /* Nothing stood here, and nothing is being put here. */
      return;
    }
    reference = bke::idprop::create(LAYER_GROUP_MATERIAL_PROP, static_cast<ID *>(nullptr))
                    .release();
    IDP_AddToGroup(properties, reference);
  }
  /* #IDP_AssignID moves the user count along with the pointer, whichever way it goes -- a null
   * included, which is how the reference is taken away again. */
  IDP_AssignID(reference, (material != nullptr) ? &material->id : nullptr, 0);
}

}  // namespace blender::bke::paint_layer
