/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cstring>
#include <iterator>

#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include <algorithm>

#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"

#include "BKE_colorband.hh"
#include "BKE_idprop.hh"
#include "BKE_paint_layers.hh"

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

static const PaintLayerKindParam brightness_contrast_params[] = {
    {"brightness", "Brightness", 0.0f, -1.0f, 1.0f},
    {"contrast", "Contrast", 0.0f, -1.0f, 1.0f},
};
static_assert(std::size(brightness_contrast_params) ==
              PAINT_LAYER_BRIGHTNESS_CONTRAST_PARAM_NUM);

/* The ramp itself lives in #MaterialPaintLayer::ramp; the schema entry is what tells the row its
 * kind has one and lets the UI find the payload. */
static const PaintLayerKindEnumItem color_ramp_alpha_items[] = {
    {0, "IGNORE", "Ignore"},
    {1, "MULTIPLY", "Use Stop Alpha"},
};
static const PaintLayerKindParam color_ramp_params[] = {
    {"affect_alpha",
     "Alpha",
     0.0f,
     0.0f,
     1.0f,
     PaintLayerKindParamType::Enum,
     color_ramp_alpha_items,
     int(std::size(color_ramp_alpha_items))},
    {"ramp", "Ramp", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Ramp},
};
static_assert(std::size(color_ramp_params) == PAINT_LAYER_COLOR_RAMP_PARAM_NUM);

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
     * from a map. As a row it lays its colour; as a content correction it lays that colour over
     * the row's; as a mask item its ramp's grey over the row's mask. */
    {MA_PAINT_LAYER_SOURCE_GENERATOR,
     MA_PAINT_LAYER_GENERATOR_GRADIENT,
     "Gradient",
     PaintLayerKindCategory::Source,
     all_roles,
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
    /* A geometry map of the object, read from the material's shared atlas: it owns channel records
     * but no map of its own, and contributes as a layer, a content correction or a mask item. */
    {MA_PAINT_LAYER_SOURCE_MESH_MAP,
     0,
     "Mesh Map",
     PaintLayerKindCategory::Source,
     all_roles},
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
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_BRIGHTNESS_CONTRAST,
     "Brightness Contrast",
     PaintLayerKindCategory::Operator,
     filter_roles,
     brightness_contrast_params,
     int(std::size(brightness_contrast_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_COLOR_RAMP,
     "Color Ramp",
     PaintLayerKindCategory::Operator,
     filter_roles,
     color_ramp_params,
     int(std::size(color_ramp_params)),
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
    /* A Curve Patch owns the channel maps its 3D control curves are stamped into, through a
     * private brush snapshot. It is therefore paintable like an Image row (NOT
     * #PAINT_LAYER_KIND_READS_SOURCE, which means "owns no map"): its maps are both the stroke
     * target during a session and the output of its bake. It exists only as a content effect: it
     * lays the stamped maps over the row it hangs on, below the row's own blend, and is never a
     * layer of the stack. The kind has no float parameters of its own: the tool settings live in
     * the snapshot. */
    {MA_PAINT_LAYER_SOURCE_CURVE_PATCH,
     0,
     "Curve Patch",
     PaintLayerKindCategory::Source,
     uint8_t(paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT)),
     nullptr,
     0,
     0,
     true},
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

PaintLayerKindParamType paint_layer_kind_param_type(const MaterialPaintLayer &row,
                                                    const int index)
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  return (param != nullptr) ? param->type : PaintLayerKindParamType::Float;
}

int paint_layer_kind_param_int_get(const MaterialPaintLayer &row, const int index)
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  if (param == nullptr || !ELEM(param->type, PaintLayerKindParamType::Int,
                               PaintLayerKindParamType::Enum))
  {
    return 0;
  }
  const IDProperty *prop = (row.system_properties != nullptr) ?
                               IDP_GetPropertyFromGroup(row.system_properties, param->name) :
                               nullptr;
  if (prop == nullptr || prop->type != IDP_INT) {
    return int(param->default_value);
  }
  return std::clamp(IDP_int_get(prop), int(param->min), int(param->max));
}

float paint_layer_kind_param_get(const MaterialPaintLayer &row, const int index)
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  if (param == nullptr) {
    return 0.0f;
  }
  if (ELEM(param->type, PaintLayerKindParamType::Int, PaintLayerKindParamType::Enum)) {
    return float(paint_layer_kind_param_int_get(row, index));
  }
  if (ELEM(param->type, PaintLayerKindParamType::Color, PaintLayerKindParamType::Ramp)) {
    /* Not a scalar: the UI and the kind's consumer read them through the typed accessors. */
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

/** The row's kind-property group, made when the row has none. */
static IDProperty *kind_param_group_ensure(MaterialPaintLayer &row)
{
  if (row.system_properties == nullptr) {
    row.system_properties = bke::idprop::create_group("kind").release();
  }
  return row.system_properties;
}

/**
 * The stored property of \a param, made with the schema's range, default and label as its UI data
 * when the row has none yet. The UI data is what lets the RNA layer show and clamp the value
 * without knowing the kind.
 */
static IDProperty *kind_param_ensure(MaterialPaintLayer &row, const PaintLayerKindParam &param)
{
  IDProperty *group = kind_param_group_ensure(row);
  IDProperty *existing = IDP_GetPropertyFromGroup(group, param.name);
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
  IDP_ReplaceInGroup(group, prop);
  return prop;
}

/** The stored integer property of \a param, made with the schema's range as UI data. */
static IDProperty *kind_param_ensure_int(MaterialPaintLayer &row,
                                         const PaintLayerKindParam &param)
{
  IDProperty *group = kind_param_group_ensure(row);
  IDProperty *existing = IDP_GetPropertyFromGroup(group, param.name);
  if (existing != nullptr && existing->type == IDP_INT) {
    return existing;
  }
  IDProperty *prop = bke::idprop::create(param.name, int(param.default_value)).release();
  IDPropertyUIDataInt *ui_data = reinterpret_cast<IDPropertyUIDataInt *>(
      IDP_ui_data_ensure(prop));
  ui_data->base.description = BLI_strdup(param.label);
  ui_data->min = ui_data->soft_min = int(param.min);
  ui_data->max = ui_data->soft_max = int(param.max);
  ui_data->default_value = int(param.default_value);
  ui_data->step = 1;
  if (param.enum_items != nullptr && param.enum_items_num > 0) {
    ui_data->enum_items_num = param.enum_items_num;
    ui_data->enum_items = MEM_new_array<IDPropertyUIDataEnumItem>(
        param.enum_items_num, __func__);
    for (const int i : IndexRange(param.enum_items_num)) {
      IDPropertyUIDataEnumItem &item = ui_data->enum_items[i];
      item.identifier = BLI_strdup(param.enum_items[i].identifier);
      item.name = BLI_strdup(param.enum_items[i].label);
      item.description = nullptr;
      item.value = param.enum_items[i].value;
      item.icon = 0;
    }
  }
  IDP_ReplaceInGroup(group, prop);
  return prop;
}

/** The stored RGBA array of a Color parameter, made at the schema default when it has none. */
static IDProperty *kind_param_ensure_color(MaterialPaintLayer &row,
                                           const PaintLayerKindParam &param)
{
  IDProperty *group = kind_param_group_ensure(row);
  IDProperty *existing = IDP_GetPropertyFromGroup(group, param.name);
  if (existing != nullptr && existing->type == IDP_ARRAY &&
      existing->subtype == IDP_FLOAT && existing->len == 4)
  {
    return existing;
  }
  float default_color[4];
  copy_v4_v4(default_color, param.default_color);
  IDProperty *prop = bke::idprop::create(param.name, Span<float>(default_color, 4)).release();
  IDP_ReplaceInGroup(group, prop);
  return prop;
}

void paint_layer_kind_params_init(MaterialPaintLayer &row)
{
  if (const PaintLayerKindInfo *info = paint_layer_kind_of(row)) {
    for (const int i : IndexRange(info->params_num)) {
      const PaintLayerKindParam &param = info->params[i];
      switch (param.type) {
        case PaintLayerKindParamType::Int:
        case PaintLayerKindParamType::Enum:
          kind_param_ensure_int(row, param);
          break;
        case PaintLayerKindParamType::Color:
          kind_param_ensure_color(row, param);
          break;
        case PaintLayerKindParamType::Ramp:
          BKE_paint_layers_ramp_ensure(row);
          break;
        case PaintLayerKindParamType::Float:
          kind_param_ensure(row, param);
          break;
      }
    }
  }
}

bool paint_layer_kind_param_set(MaterialPaintLayer &row, const int index, const float value)
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  if (param == nullptr || param->type != PaintLayerKindParamType::Float) {
    return false;
  }
  IDP_float_set(kind_param_ensure(row, *param), clamp_f(value, param->min, param->max));
  return true;
}

bool paint_layer_kind_param_int_set(MaterialPaintLayer &row, const int index, const int value)
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  if (param == nullptr || !ELEM(param->type, PaintLayerKindParamType::Int,
                               PaintLayerKindParamType::Enum))
  {
    return false;
  }
  IDP_int_set(kind_param_ensure_int(row, *param),
              std::clamp(value, int(param->min), int(param->max)));
  return true;
}

void paint_layer_kind_param_color_get(const MaterialPaintLayer &row,
                                      const int index,
                                      float r_color[4])
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  if (param == nullptr || param->type != PaintLayerKindParamType::Color) {
    zero_v4(r_color);
    return;
  }
  const IDProperty *prop = (row.system_properties != nullptr) ?
                               IDP_GetPropertyFromGroup(row.system_properties, param->name) :
                               nullptr;
  if (prop == nullptr || prop->type != IDP_ARRAY || prop->subtype != IDP_FLOAT ||
      prop->len != 4)
  {
    copy_v4_v4(r_color, param->default_color);
    return;
  }
  copy_v4_v4(r_color, IDP_array_float_get(prop));
}

bool paint_layer_kind_param_color_set(MaterialPaintLayer &row,
                                      const int index,
                                      const float color[4])
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  if (param == nullptr || param->type != PaintLayerKindParamType::Color) {
    return false;
  }
  IDProperty *prop = kind_param_ensure_color(row, *param);
  copy_v4_v4(IDP_array_float_get(prop), color);
  return true;
}

bool BKE_paint_layers_ramp_supported(const MaterialPaintLayer &row)
{
  return paint_layer_kind_has_ramp_param(row);
}

/** Whether \a data is \a band itself or one of its stops. */
static bool paint_layer_ramp_owns_data(const ColorBand &band, const void *data)
{
  if (data == &band) {
    return true;
  }
  for (const int i : IndexRange(band.tot)) {
    if (data == &band.data[i]) {
      return true;
    }
  }
  return false;
}

static bool paint_layer_ramp_edited_walk(MaterialPaintLayer &row, const void *data)
{
  if (row.ramp != nullptr && paint_layer_ramp_owns_data(row.ramp->band, data)) {
    return true;
  }
  for (MaterialPaintLayer &item : row.mask_stack) {
    if (paint_layer_ramp_edited_walk(item, data)) {
      return true;
    }
  }
  for (MaterialPaintLayer &effect : row.effects) {
    if (paint_layer_ramp_edited_walk(effect, data)) {
      return true;
    }
  }
  for (MaterialPaintLayer &child : row.children) {
    if (paint_layer_ramp_edited_walk(child, data)) {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_ramp_edited(Material &ma, const void *data)
{
  if (data == nullptr) {
    return false;
  }
  bool found = false;
  for (MaterialPaintLayer &row : ma.paint_layers) {
    if (paint_layer_ramp_edited_walk(row, data)) {
      found = true;
      break;
    }
  }
  if (found) {
    /* The stops are in the cached bake's hash like a gradient's: without the stale flag a baked row
     * or folder that holds the filter keeps showing the old ramp. */
    ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
    BKE_paint_layers_tag_edited(ma);
  }
  return found;
}

bool paint_layer_kind_has_ramp_param(const MaterialPaintLayer &row)
{
  const PaintLayerKindInfo *info = paint_layer_kind_of(row);
  if (info == nullptr) {
    return false;
  }
  for (const int i : IndexRange(info->params_num)) {
    if (info->params[i].type == PaintLayerKindParamType::Ramp) {
      return true;
    }
  }
  return false;
}

MaterialPaintLayerRamp *BKE_paint_layers_ramp_ensure(MaterialPaintLayer &row)
{
  if (!paint_layer_kind_has_ramp_param(row)) {
    return nullptr;
  }
  if (row.ramp == nullptr) {
    row.ramp = MEM_new<MaterialPaintLayerRamp>(__func__);
    /* Black to white, linear: the identity on a grey. The gradient brush's default starts its black
     * stop at the middle with a B-spline, which maps every colour below half luminance to black. */
    BKE_colorband_init(&row.ramp->band, true);
  }
  return row.ramp;
}

const ColorBand *BKE_paint_layers_ramp_get(const MaterialPaintLayer &row)
{
  return (row.ramp != nullptr) ? &row.ramp->band : nullptr;
}

float paint_layer_color_ramp_apply(const ColorBand &band, const bool use_alpha, float rgb[3])
{
  /* The RGB to BW node's scene-linear Rec.709 luminance, matching the graph's #SH_NODE_RGBTOBW. */
  const float value = clamp_f(
      0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2], 0.0f, 1.0f);
  float ramp_color[4];
  if (BKE_colorband_evaluate(&band, value, ramp_color)) {
    copy_v3_v3(rgb, ramp_color);
    /* The stop's alpha scales the strength and not the layer's alpha: a filter lays no alpha. */
    return use_alpha ? clamp_f(ramp_color[3], 0.0f, 1.0f) : 1.0f;
  }
  return 1.0f;
}

uint64_t paint_layer_ramp_hash(const ColorBand &band)
{
  uint64_t hash = 1469598103934665603ull;
  const auto mix_u32 = [&](const uint32_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  const auto mix_float = [&](const float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    mix_u32(bits);
  };
  mix_u32(uint32_t(band.tot));
  mix_u32(uint32_t(band.ipotype));
  mix_u32(uint32_t(band.ipotype_hue));
  mix_u32(uint32_t(band.color_mode));
  for (const int i : IndexRange(band.tot)) {
    const CBData &element = band.data[i];
    mix_float(element.pos);
    mix_float(element.r);
    mix_float(element.g);
    mix_float(element.b);
    mix_float(element.a);
  }
  return hash;
}

int paint_layer_kind_param_index(const MaterialPaintLayer &row, const char *name)
{
  const PaintLayerKindInfo *info = paint_layer_kind_of(row);
  if (info == nullptr) {
    return -1;
  }
  for (const int i : IndexRange(info->params_num)) {
    if (STREQ(info->params[i].name, name)) {
      return i;
    }
  }
  return -1;
}

}  // namespace blender
