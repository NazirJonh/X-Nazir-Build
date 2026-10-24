/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <iterator>

#include "BLI_index_range.hh"
#include <algorithm>

#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_string.h"

#include "DNA_ID.h"

#include "BKE_idprop.hh"

#include "paint_layers_kind.hh"

namespace blender {

static const PaintLayerKindParam levels_params[] = {
    {"in_black", "Input Black", 0.0f, 0.0f, 1.0f},
    {"in_white", "Input White", 1.0f, 0.0f, 1.0f},
    /* Never zero: a gamma of zero has no inverse. */
    {"gamma", "Gamma", 1.0f, 0.01f, 10.0f},
    {"out_black", "Output Black", 0.0f, 0.0f, 1.0f},
    {"out_white", "Output White", 1.0f, 0.0f, 1.0f},
};
static_assert(std::size(levels_params) == PAINT_LAYER_LEVELS_PARAM_NUM);

static const PaintLayerKindParam hsl_params[] = {
    /* The node's own convention: 0.5 leaves the hue where it is. */
    {"hue", "Hue", 0.5f, 0.0f, 1.0f},
    {"saturation", "Saturation", 1.0f, 0.0f, 2.0f},
    {"value", "Value", 1.0f, 0.0f, 2.0f},
};
static_assert(std::size(hsl_params) == PAINT_LAYER_HSL_PARAM_NUM);

static const PaintLayerKindParam coverage_params[] = {
    /* Half the range: a pick matches the whole region of its colour, not only the exact texel value. */
    {"tolerance", "Tolerance", 0.5f, 0.0f, 1.0f},
    {"softness", "Softness", 0.0f, 0.0f, 1.0f},
};
static_assert(std::size(coverage_params) == PAINT_LAYER_COVERAGE_PARAM_NUM);

void paint_layer_hsl_apply(float rgb[3], const float params[PAINT_LAYER_HSL_PARAM_NUM])
{
  float hsv[3];
  rgb_to_hsv_v(rgb, hsv);
  const float turned = hsv[0] + params[PAINT_LAYER_HSL_HUE] + 0.5f;
  hsv[0] = turned - floorf(turned);
  hsv[1] = clamp_f(hsv[1] * params[PAINT_LAYER_HSL_SATURATION], 0.0f, 1.0f);
  hsv[2] *= params[PAINT_LAYER_HSL_VALUE];
  hsv_to_rgb_v(hsv, rgb);
  for (int i = 0; i < 3; i++) {
    rgb[i] = std::max(rgb[i], 0.0f);
  }
}

static constexpr uint8_t all_roles = paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER) |
                                     paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT) |
                                     paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM);

/* As a mask item a filter works on the mask's coverage, a single grey: it has no channels there. */
static constexpr uint8_t filter_roles = paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER) |
                                        paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT) |
                                        paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM);

/* A map-coverage operator only exists as an item of a row's mask; it is never a row or a content
 * correction of its own. */
static constexpr uint8_t mask_item_roles = paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM);

/* New kinds are added here. A kind that needs the row bake is registered with a `TODO:` naming
 * it, so what is deferred shows up in a search of this file. */
static const PaintLayerKindInfo kinds[] = {
    {MA_PAINT_LAYER_SOURCE_IMAGE, 0, "Image", PaintLayerKindCategory::Source, all_roles},
    {MA_PAINT_LAYER_SOURCE_CONSTANT,
     0,
     "Constant",
     PaintLayerKindCategory::Source,
     all_roles,
     nullptr,
     0,
     PAINT_LAYER_KIND_FLAT_VALUE},
    /* A material is read, not painted: the row has no records and lays what the source shows. A
     * row of the stack, a content correction laid over one, or a mask correction whose grey is the
     * source's colour. */
    {MA_PAINT_LAYER_SOURCE_MATERIAL,
     0,
     "Material",
     PaintLayerKindCategory::Source,
     uint8_t(paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER) |
             paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT) |
             paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM)),
     nullptr,
     0,
     PAINT_LAYER_KIND_READS_SOURCE},
    /* A procedural generator: its colour and coverage come from shader nodes reading the UV, not
     * from a map. The first vertical slice is the Layer role; corrections and mask items follow
     * once their graph and CPU paths are in place. */
    {MA_PAINT_LAYER_SOURCE_GENERATOR,
     MA_PAINT_LAYER_GENERATOR_GRADIENT,
     "Gradient",
     PaintLayerKindCategory::Source,
     paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER),
     nullptr,
     0,
     PAINT_LAYER_KIND_GENERATES,
     /* The kind owns no map, so a stroke cannot paint into it. */
     false},
    /* A folder owns children, which corrections and mask items never have. */
    {MA_PAINT_LAYER_SOURCE_STACK,
     0,
     "Stack",
     PaintLayerKindCategory::Container,
     paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER)},
    /* A filter changes the colour it reads and owns none: as a correction it works on its row's
     * colour, as a layer (an adjustment) on the stack below it. It has no coverage a mask item could
     * be. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_INVERT,
     "Invert",
     PaintLayerKindCategory::Operator,
     filter_roles,
     nullptr,
     0,
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_LEVELS,
     "Levels",
     PaintLayerKindCategory::Operator,
     filter_roles,
     levels_params,
     int(std::size(levels_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_HSL,
     "Hue Saturation Value",
     PaintLayerKindCategory::Operator,
     filter_roles,
     hsl_params,
     int(std::size(hsl_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    /* A map-coverage operator: the map of its Base Color record is an external ID Map, so the map
     * is reference-only and a stroke must not turn it into a paint target. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_COLOR_ID,
     "Color ID",
     PaintLayerKindCategory::Operator,
     mask_item_roles,
     coverage_params,
     int(std::size(coverage_params)),
     PAINT_LAYER_KIND_MAP_COVERAGE,
     false},
    /* A map-coverage operator whose map is written by the UV Island capture, so it is paintable. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_UV_ISLAND,
     "UV Island",
     PaintLayerKindCategory::Operator,
     mask_item_roles,
     coverage_params,
     int(std::size(coverage_params)),
     PAINT_LAYER_KIND_MAP_COVERAGE,
     true},
    /* TODO: Gradient Map filter, needs a ColorBand parameter. */
    /* TODO: Blur filter, needs the row bake (Phase 3). */
    /* TODO: Sharpen filter, needs the row bake (Phase 3). */
    /* TODO: Edge Detect filter, needs the row bake (Phase 3). */
    /* An Anchor Point is an inert marker inside a row: it publishes the content after the
     * corrections above it (or the mask after the mask items above it) and lays nothing itself. It
     * has no parameters and no channel records. */
    {MA_PAINT_LAYER_SOURCE_ANCHOR_POINT,
     MA_PAINT_LAYER_ANCHOR_POINT_MARKER,
     "Anchor Point",
     PaintLayerKindCategory::Source,
     uint8_t(paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT) |
             paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM)),
     nullptr,
     0,
     PAINT_LAYER_KIND_ANCHOR_POINT_MARK},
    /* An Anchor Point Reader lays the slice a marker publishes. It owns no map and carries no
     * channel records; what it shows is decided from the point it names. As a row of the stack, a
     * content correction or a mask item, it lays the slice with its own opacity and blend. */
    {MA_PAINT_LAYER_SOURCE_ANCHOR_POINT,
     MA_PAINT_LAYER_ANCHOR_POINT_READER,
     "Anchor Point Reader",
     PaintLayerKindCategory::Source,
     uint8_t(all_roles),
     nullptr,
     0,
     PAINT_LAYER_KIND_READS_ANCHOR_POINT},
};

const PaintLayerKindInfo *paint_layer_kind_find(const int8_t source, const int16_t subtype)
{
  for (const PaintLayerKindInfo &info : kinds) {
    if (info.source == source && info.subtype == subtype) {
      return &info;
    }
  }
  return nullptr;
}

bool paint_layer_kind_role_allowed(const MaterialPaintLayer &row)
{
  const PaintLayerKindInfo *info = paint_layer_kind_of(row);
  return info != nullptr && (info->allowed_roles & paint_layer_role_bit(row.role)) != 0;
}

/** The schema entry \a index of \a row's kind, or null when the kind has no such parameter. */
static const PaintLayerKindParam *kind_param_find(const MaterialPaintLayer &row, const int index)
{
  const PaintLayerKindInfo *info = paint_layer_kind_of(row);
  if (info == nullptr || index < 0 || index >= info->params_num) {
    return nullptr;
  }
  return &info->params[index];
}

float paint_layer_kind_param_get(const MaterialPaintLayer &row, const int index)
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  if (param == nullptr) {
    return 0.0f;
  }
  const IDProperty *prop = (row.system_properties != nullptr) ?
                               IDP_GetPropertyFromGroup(row.system_properties, param->name) :
                               nullptr;
  if (prop == nullptr || prop->type != IDP_FLOAT) {
    return param->default_value;
  }
  return clamp_f(IDP_float_get(prop), param->min, param->max);
}

/**
 * The stored property of \a param, made with the schema's range, default and label as its UI data
 * when the row has none yet. The UI data is what lets the RNA layer show and clamp the value
 * without knowing the kind.
 */
static IDProperty *kind_param_ensure(MaterialPaintLayer &row, const PaintLayerKindParam &param)
{
  if (row.system_properties == nullptr) {
    row.system_properties = bke::idprop::create_group("kind").release();
  }
  IDProperty *existing = IDP_GetPropertyFromGroup(row.system_properties, param.name);
  if (existing != nullptr && existing->type == IDP_FLOAT) {
    return existing;
  }
  IDProperty *prop = bke::idprop::create(param.name, param.default_value).release();
  IDPropertyUIDataFloat *ui_data = reinterpret_cast<IDPropertyUIDataFloat *>(
      IDP_ui_data_ensure(prop));
  ui_data->base.description = BLI_strdup(param.label);
  ui_data->min = ui_data->soft_min = param.min;
  ui_data->max = ui_data->soft_max = param.max;
  ui_data->default_value = param.default_value;
  ui_data->step = 0.1f;
  ui_data->precision = 3;
  IDP_ReplaceInGroup(row.system_properties, prop);
  return prop;
}

void paint_layer_kind_params_init(MaterialPaintLayer &row)
{
  if (const PaintLayerKindInfo *info = paint_layer_kind_of(row)) {
    for (const int i : IndexRange(info->params_num)) {
      kind_param_ensure(row, info->params[i]);
    }
  }
}

bool paint_layer_kind_param_set(MaterialPaintLayer &row, const int index, const float value)
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  if (param == nullptr) {
    return false;
  }
  IDP_float_set(kind_param_ensure(row, *param), clamp_f(value, param->min, param->max));
  return true;
}

}  // namespace blender
