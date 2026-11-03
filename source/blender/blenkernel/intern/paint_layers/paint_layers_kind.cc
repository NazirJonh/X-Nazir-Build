/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cstring>
#include <iterator>

#include "BLI_hash.hh"
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
#include "BKE_colortools.hh"
#include "BKE_idprop.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_material_composite.hh"

#include "DEG_depsgraph.hh"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

static_assert(PAINT_LAYER_COLOR_BALANCE_FOLDED_NUM <= PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM);

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

static const PaintLayerKindParam posterize_params[] = {
    /* Whole steps: two is the smallest count that shows a step. */
    {"steps", "Steps", 4.0f, 2.0f, 64.0f, PaintLayerKindParamType::Int},
};
static_assert(std::size(posterize_params) == PAINT_LAYER_POSTERIZE_PARAM_NUM);

static const PaintLayerKindEnumItem math_operation_items[] = {
    {PAINT_LAYER_MATH_OP_ADD, "ADD", "Add"},
    {PAINT_LAYER_MATH_OP_SUBTRACT, "SUBTRACT", "Subtract"},
    {PAINT_LAYER_MATH_OP_MULTIPLY, "MULTIPLY", "Multiply"},
    {PAINT_LAYER_MATH_OP_DIVIDE, "DIVIDE", "Divide"},
    {PAINT_LAYER_MATH_OP_MINIMUM, "MINIMUM", "Minimum"},
    {PAINT_LAYER_MATH_OP_MAXIMUM, "MAXIMUM", "Maximum"},
    {PAINT_LAYER_MATH_OP_POWER, "POWER", "Power"},
    {PAINT_LAYER_MATH_OP_LOGARITHM, "LOGARITHM", "Logarithm"},
};
static_assert(int(std::size(math_operation_items)) - 1 == PAINT_LAYER_MATH_OP_LOGARITHM);

static const PaintLayerKindParam math_params[] = {
    /* The generated node's operation is decided when the graph is built, so an edit of this is a
     * shape edit and the tree is rebuilt; the operands stay value inputs. */
    {"operation",
     "Operation",
     float(PAINT_LAYER_MATH_OP_ADD),
     0.0f,
     float(PAINT_LAYER_MATH_OP_LOGARITHM),
     PaintLayerKindParamType::Enum,
     math_operation_items,
     int(std::size(math_operation_items))},
    /* The Math nodes' Use Clamp: folding the result into [0, 1] is the shape the graph is built
     * with, so an edit of it is a shape edit too, not a value the graph reads. A checkbox: the
     * schema's min/max/default read as the Boolean's two ends. */
    {"clamp",
     "Clamp",
     0.0f,
     0.0f,
     1.0f,
     PaintLayerKindParamType::Bool},
    /* One operand per colour channel, wide enough to reach a multiply of two: the operands are
     * plain constants, not gains. */
    {"operand_r", "R", 0.0f, -10.0f, 10.0f},
    {"operand_g", "G", 0.0f, -10.0f, 10.0f},
    {"operand_b", "B", 0.0f, -10.0f, 10.0f},
};
static_assert(std::size(math_params) == PAINT_LAYER_MATH_PARAM_NUM);

static const PaintLayerKindParam threshold_params[] = {
    {"threshold", "Threshold", 0.5f, 0.0f, 1.0f},
};
static_assert(std::size(threshold_params) == PAINT_LAYER_THRESHOLD_PARAM_NUM);

static const PaintLayerKindParam clamp_params[] = {
    {"min", "Min", 0.0f, 0.0f, 1.0f},
    {"max", "Max", 1.0f, 0.0f, 1.0f},
};
static_assert(std::size(clamp_params) == PAINT_LAYER_CLAMP_PARAM_NUM);

static const PaintLayerKindEnumItem blur_shape_items[] = {
    {PAINT_LAYER_BLUR_BOX, "BOX", "Box"},
    {PAINT_LAYER_BLUR_GAUSSIAN, "GAUSSIAN", "Gaussian"},
};
static_assert(int(std::size(blur_shape_items)) - 1 == PAINT_LAYER_BLUR_GAUSSIAN);

static const PaintLayerKindParam blur_params[] = {
    /* The kernel is chosen when the effect is computed, so an edit of this is a shape edit and
     * the caches rebuild; the radius stays a value next to it. */
    {"shape",
     "Shape",
     float(PAINT_LAYER_BLUR_GAUSSIAN),
     0.0f,
     float(PAINT_LAYER_BLUR_GAUSSIAN),
     PaintLayerKindParamType::Enum,
     blur_shape_items,
     int(std::size(blur_shape_items))},
    /* Whole texels: the kernel's half reach. One box pass carries it; a gaussian spends it over
     * three. */
    {"radius", "Radius", 4.0f, 1.0f, 64.0f, PaintLayerKindParamType::Int},
};
static_assert(std::size(blur_params) == PAINT_LAYER_BLUR_PARAM_NUM);

static const PaintLayerKindParam sharpen_params[] = {
    /* Whole texels: the half reach of the box the detail is read against. */
    {"radius", "Radius", 2.0f, 1.0f, 64.0f, PaintLayerKindParamType::Int},
    /* The unsharp weight: how much of what the blur removed comes back. */
    {"amount", "Amount", 0.5f, 0.0f, 2.0f},
};
static_assert(std::size(sharpen_params) == PAINT_LAYER_SHARPEN_PARAM_NUM);

static const PaintLayerKindEnumItem high_pass_output_items[] = {
    {PAINT_LAYER_HIGH_PASS_OUTPUT_COLOR, "COLOR", "Color"},
    {PAINT_LAYER_HIGH_PASS_OUTPUT_GRAYSCALE, "GRAYSCALE", "Grayscale"},
};
static_assert(int(std::size(high_pass_output_items)) - 1 == PAINT_LAYER_HIGH_PASS_OUTPUT_GRAYSCALE);

static const PaintLayerKindParam high_pass_params[] = {
    /* Whole texels: the half reach of the box the detail is read against. Wider than the other
     * neighborhood kinds, since a high pass keeps only what is finer than this reach and a 4K map
     * needs a reach of hundreds of texels to keep its larger shapes. */
    {"radius", "Radius", 4.0f, 1.0f, 256.0f, PaintLayerKindParamType::Int},
    /* How much of the detail survives: one keeps it all, zero flattens to the mid grey, above one
     * boosts it (the detail is small against the grey it sits on). */
    {"contrast", "Contrast", 1.0f, 0.0f, 10.0f},
    /* Grayscale takes the row's luminance before the filter, for a black and white detail map. It
     * comes last so the positions of the parameters of an older file stay as they were. */
    {"output",
     "Output",
     float(PAINT_LAYER_HIGH_PASS_OUTPUT_COLOR),
     0.0f,
     float(PAINT_LAYER_HIGH_PASS_OUTPUT_GRAYSCALE),
     PaintLayerKindParamType::Enum,
     high_pass_output_items,
     int(std::size(high_pass_output_items))},
};
static_assert(std::size(high_pass_params) == PAINT_LAYER_HIGH_PASS_PARAM_NUM);

static const PaintLayerKindEnumItem erode_dilate_mode_items[] = {
    {PAINT_LAYER_ERODE_DILATE_ERODE, "ERODE", "Erode"},
    {PAINT_LAYER_ERODE_DILATE_DILATE, "DILATE", "Dilate"},
};
static_assert(int(std::size(erode_dilate_mode_items)) - 1 == PAINT_LAYER_ERODE_DILATE_DILATE);

static const PaintLayerKindParam erode_dilate_params[] = {
    /* The direction is part of what the effect computes, so an edit of it is a shape edit and the
     * caches re-key; the kernel reads no node either way. */
    {"mode",
     "Mode",
     float(PAINT_LAYER_ERODE_DILATE_ERODE),
     0.0f,
     float(PAINT_LAYER_ERODE_DILATE_DILATE),
     PaintLayerKindParamType::Enum,
     erode_dilate_mode_items,
     int(std::size(erode_dilate_mode_items))},
    /* The half reach of the square window: the extreme is taken over (2r+1)^2 texels. */
    {"radius", "Radius", 2.0f, 1.0f, 64.0f, PaintLayerKindParamType::Int},
};
static_assert(std::size(erode_dilate_params) == PAINT_LAYER_ERODE_DILATE_PARAM_NUM);

/* The curves live in #MaterialPaintLayer::curves, like the ramp's band in its payload. */
static const PaintLayerKindParam curves_params[] = {
    {"curves", "Curves", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Curves},
};

/* The balance lives in #MaterialPaintLayer::color_balance; the schema entry is what tells the row
 * its kind has one and lets the UI find the payload. The two methods fold into the same 12 numbers
 * (#paint_layer_color_balance_fold), so the kind carries no per-method parameters. */
static const PaintLayerKindParam color_balance_params[] = {
    {"color_balance", "Color Balance", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::ColorBalance},
};

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

/* A neighborhood effect only corrects a row's colour: the map it reads is the row's own, and an
 * adjustment or a mask item has no map of the row's for it to read. */
static constexpr uint8_t effect_only_roles = paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT);

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
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_POSTERIZE,
     "Posterize",
     PaintLayerKindCategory::Operator,
     filter_roles,
     posterize_params,
     int(std::size(posterize_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_MATH,
     "Math",
     PaintLayerKindCategory::Operator,
     filter_roles,
     math_params,
     int(std::size(math_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_THRESHOLD,
     "Threshold",
     PaintLayerKindCategory::Operator,
     filter_roles,
     threshold_params,
     int(std::size(threshold_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_CLAMP,
     "Clamp",
     PaintLayerKindCategory::Operator,
     filter_roles,
     clamp_params,
     int(std::size(clamp_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_CURVES,
     "RGB Curves",
     PaintLayerKindCategory::Operator,
     filter_roles,
     curves_params,
     int(std::size(curves_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_COLOR_BALANCE,
     "Color Balance",
     PaintLayerKindCategory::Operator,
     filter_roles,
     color_balance_params,
     int(std::size(color_balance_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    /* A neighborhood effect reads a neighbourhood of the row's map, which the live graph cannot
     * express: it exists only as a correction of a row's colour, the effect cache computes it, and
     * a row that carries one is baked for the live graph. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_BLUR,
     "Blur",
     PaintLayerKindCategory::Operator,
     effect_only_roles,
     blur_params,
     int(std::size(blur_params)),
     uint8_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE)},
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_SHARPEN,
     "Sharpen",
     PaintLayerKindCategory::Operator,
     effect_only_roles,
     sharpen_params,
     int(std::size(sharpen_params)),
     uint8_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE)},
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_HIGH_PASS,
     "High Pass",
     PaintLayerKindCategory::Operator,
     effect_only_roles,
     high_pass_params,
     int(std::size(high_pass_params)),
     uint8_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE)},
    /* The Sobel gradient magnitude: a full-range step reads as one, a flat area as zero. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_EDGE_DETECT,
     "Edge Detect",
     PaintLayerKindCategory::Operator,
     effect_only_roles,
     nullptr,
     0,
     uint8_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE)},
    /* A diagonal 3x3 lighting kernel: a flat colour is its own result, an edge takes the light. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_EMBOSS,
     "Emboss",
     PaintLayerKindCategory::Operator,
     effect_only_roles,
     nullptr,
     0,
     uint8_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE)},
    /* The extreme of a square window per channel: min and max are separable, so the kernel is one
     * pass per axis like the box blur's. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_ERODE_DILATE,
     "Erode / Dilate",
     PaintLayerKindCategory::Operator,
     effect_only_roles,
     erode_dilate_params,
     int(std::size(erode_dilate_params)),
     uint8_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE)},
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

bool paint_layer_kind_param_bool_get(const MaterialPaintLayer &row, const int index)
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  if (param == nullptr || param->type != PaintLayerKindParamType::Bool) {
    return false;
  }
  const IDProperty *prop = (row.system_properties != nullptr) ?
                               IDP_GetPropertyFromGroup(row.system_properties, param->name) :
                               nullptr;
  if (prop == nullptr || prop->type != IDP_BOOLEAN) {
    return param->default_value != 0.0f;
  }
  return IDP_bool_get(prop);
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
  if (param->type == PaintLayerKindParamType::Bool) {
    /* The scalar readers (the composite's filter_params, the group inputs, the GPU uniforms) stay
     * float: a Boolean reads as 0.0f or 1.0f through the same path. */
    return float(paint_layer_kind_param_bool_get(row, index));
  }
  if (ELEM(param->type,
           PaintLayerKindParamType::Color,
           PaintLayerKindParamType::Ramp,
           PaintLayerKindParamType::Curves,
           PaintLayerKindParamType::ColorBalance))
  {
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

/** The stored boolean property of \a param, made with the schema default as its UI data. */
static IDProperty *kind_param_ensure_bool(MaterialPaintLayer &row,
                                          const PaintLayerKindParam &param)
{
  IDProperty *group = kind_param_group_ensure(row);
  IDProperty *existing = IDP_GetPropertyFromGroup(group, param.name);
  if (existing != nullptr && existing->type == IDP_BOOLEAN) {
    return existing;
  }
  const bool default_on = param.default_value != 0.0f;
  IDProperty *prop = bke::idprop::create_bool(param.name, default_on).release();
  IDPropertyUIDataBool *ui_data = reinterpret_cast<IDPropertyUIDataBool *>(
      IDP_ui_data_ensure(prop));
  ui_data->base.description = BLI_strdup(param.label);
  ui_data->default_value = default_on;
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
        case PaintLayerKindParamType::Curves:
          BKE_paint_layers_curves_ensure(row);
          break;
        case PaintLayerKindParamType::ColorBalance:
          BKE_paint_layers_color_balance_ensure(row);
          break;
        case PaintLayerKindParamType::Bool:
          kind_param_ensure_bool(row, param);
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

bool paint_layer_kind_param_bool_set(MaterialPaintLayer &row, const int index, const bool value)
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  if (param == nullptr || param->type != PaintLayerKindParamType::Bool) {
    return false;
  }
  IDP_bool_set(kind_param_ensure_bool(row, *param), value);
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

/** Whether \a owns is true of the row itself, one of its mask items, its effects or its children. */
template<typename Owns> static bool paint_layer_payload_edited_walk(MaterialPaintLayer &row,
                                                                    const Owns owns)
{
  if (owns(row)) {
    return true;
  }
  for (MaterialPaintLayer &item : row.mask_stack) {
    if (paint_layer_payload_edited_walk(item, owns)) {
      return true;
    }
  }
  for (MaterialPaintLayer &effect : row.effects) {
    if (paint_layer_payload_edited_walk(effect, owns)) {
      return true;
    }
  }
  for (MaterialPaintLayer &child : row.children) {
    if (paint_layer_payload_edited_walk(child, owns)) {
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
  const auto owns = [&](const MaterialPaintLayer &row) {
    return row.ramp != nullptr && paint_layer_ramp_owns_data(row.ramp->band, data);
  };
  bool found = false;
  for (MaterialPaintLayer &row : ma.paint_layers) {
    if (paint_layer_payload_edited_walk(row, owns)) {
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

bool paint_layer_kind_has_curves_param(const MaterialPaintLayer &row)
{
  const PaintLayerKindInfo *info = paint_layer_kind_of(row);
  if (info == nullptr) {
    return false;
  }
  for (const int i : IndexRange(info->params_num)) {
    if (info->params[i].type == PaintLayerKindParamType::Curves) {
      return true;
    }
  }
  return false;
}

MaterialPaintLayerCurves *BKE_paint_layers_curves_ensure(MaterialPaintLayer &row)
{
  if (!paint_layer_kind_has_curves_param(row)) {
    return nullptr;
  }
  if (row.curves == nullptr) {
    row.curves = MEM_new<MaterialPaintLayerCurves>(__func__);
    /* Four curves (R, G, B and the combined one), flat over 0..1, with the tables built for the
     * CPU composite to read. */
    BKE_curvemapping_set_defaults(&row.curves->curve_mapping, 4, 0.0f, 0.0f, 1.0f, 1.0f, HD_AUTO);
    BKE_curvemapping_init(&row.curves->curve_mapping);
  }
  return row.curves;
}

const CurveMapping *BKE_paint_layers_curves_get(const MaterialPaintLayer &row)
{
  return (row.curves != nullptr) ? &row.curves->curve_mapping : nullptr;
}

bool BKE_paint_layers_curves_supported(const MaterialPaintLayer &row)
{
  return paint_layer_kind_has_curves_param(row);
}

/** The depth of the live-drag scope #BKE_paint_layers_curves_live_begin opened on this thread. */
static thread_local int curves_live_depth = 0;

void BKE_paint_layers_curves_live_begin()
{
  curves_live_depth++;
}

void BKE_paint_layers_curves_live_end()
{
  /* A missing end would flip every later edit to the live path, so an unbalanced call is loud. */
  BLI_assert(curves_live_depth > 0);
  curves_live_depth--;
}

bool BKE_paint_layers_curves_edited(Main &bmain, Material &ma, const void *data)
{
  if (data == nullptr) {
    return false;
  }
  const auto owns = [&](const MaterialPaintLayer &row) {
    return row.curves != nullptr && data == &row.curves->curve_mapping;
  };
  bool found = false;
  for (MaterialPaintLayer &row : ma.paint_layers) {
    if (paint_layer_payload_edited_walk(row, owns)) {
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }
  /* No composite cache invalidation here: the curves are in the composite stack's hash, so the
   * channel the filter works in re-composites by itself on the next read. Dropping the whole
   * material's cache re-rendered every channel at full size (about 0.9 s at 4K, measured) for a
   * shape only one of them reads. */
  /* The curves' shape is part of the row's state the generated node carries a copy of, so the
   * cheap path is the in-place sync (#paint_layers_curves_sync): the node's storage re-copies the
   * mapping and no group is rebuilt. A live drag (inside #BKE_paint_layers_curves_live_begin)
   * takes that sync but none of the bake flags — re-baking on every point move would re-render
   * the baked maps for a shape the release re-checks anyway. The final edit — the release, or any
   * non-drag update — marks the bake stale like a ramp's stops do: without it a baked row that
   * holds the filter keeps showing the old curves. */
  const bool live = curves_live_depth > 0;
  if (!live) {
    ma.paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_CANCELLED;
    ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  }
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  if (!bke::paint_layers::paint_layers_curves_sync(ma, bmain)) {
    /* The node is missing, mistyped or the group no longer matches the description: the sync
     * cannot fix that in place, so the regeneration point rebuilds it. The old behaviour — a tag,
     * no failure — keeps a drag alive when a tree is hand-edited underneath it. */
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
    DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  }
  return found;
}

void paint_layer_curves_apply(const CurveMapping &curve_mapping, float rgb[3])
{
  /* The RGB Curves shader node's own function: each channel through its curve, with no white
   * balancing — the node's GPU and multi-function evaluations ignore the levels ("shader nodes
   * don't do white balancing"), so the CPU does too. The tables are built
   * (#BKE_curvemapping_init) when the payload is made, copied or loaded. */
  BKE_curvemapping_evaluateRGBF(&curve_mapping, rgb, rgb);
}

void paint_layer_curves_lut_build(const CurveMapping &curve_mapping,
                                  Vector<float4> &r_texels,
                                  int &r_width,
                                  int &r_height)
{
  float *band_values = nullptr;
  int band_size = 0;
  BKE_curvemapping_table_RGBA(&curve_mapping, &band_values, &band_size);

  /* Row 1 holds four texels of four values each; a table shorter than that would clip them. */
  r_width = std::max(band_size, CM_TOT);
  r_height = 2;
  r_texels.resize(size_t(r_width) * size_t(r_height));
  for (const int i : IndexRange(band_size)) {
    r_texels[i] = float4(band_values[i * 4 + 0],
                         band_values[i * 4 + 1],
                         band_values[i * 4 + 2],
                         band_values[i * 4 + 3]);
  }
  MEM_delete(band_values);

  float start_slopes[CM_TOT];
  float end_slopes[CM_TOT];
  float range_minimums[CM_TOT];
  float range_dividers[CM_TOT];
  BKE_curvemapping_compute_slopes(&curve_mapping, start_slopes, end_slopes);
  BKE_curvemapping_get_range_minimums(&curve_mapping, range_minimums);
  BKE_curvemapping_compute_range_dividers(&curve_mapping, range_dividers);
  const int row = r_width;
  r_texels[row + 0] = float4(start_slopes[0], start_slopes[1], start_slopes[2], start_slopes[3]);
  r_texels[row + 1] = float4(end_slopes[0], end_slopes[1], end_slopes[2], end_slopes[3]);
  r_texels[row + 2] = float4(
      range_minimums[0], range_minimums[1], range_minimums[2], range_minimums[3]);
  r_texels[row + 3] = float4(
      range_dividers[0], range_dividers[1], range_dividers[2], range_dividers[3]);
}

namespace {

/** One component of row 0 at the normalized table coordinate \a parameter, linearly interpolated. */
float curves_lut_component(const Span<float4> texels, const int width, float parameter, int component)
{
  /* The table spans `CM_TABLE` intervals over the normalized range, as `CurveMap::range` has it. */
  const float last = float(CM_TABLE);
  const float index = parameter * last;
  const float low = std::floor(index);
  const float fraction = index - low;
  const int i0 = std::clamp(int(low), 0, width - 1);
  const int i1 = std::clamp(int(low) + 1, 0, width - 1);
  return (1.0f - fraction) * texels[i0][component] + fraction * texels[i1][component];
}

/** The GLSL node's extrapolate_if_needed: the table's edge value plus the slope outside [0, 1]. */
float curves_lut_extrapolate(float parameter, float value, float start_slope, float end_slope)
{
  if (parameter < 0.0f) {
    return value + parameter * start_slope;
  }
  if (parameter > 1.0f) {
    return value + (parameter - 1.0f) * end_slope;
  }
  return value;
}

}  // namespace

void paint_layer_curves_lut_eval(const Span<float4> texels, const int width, float rgb[3])
{
  /* The layout #paint_layer_curves_lut_build writes: row 1's four texels are the per-curve slopes
   * and ranges, indexed by the curve map (R, G, B, combined). */
  const int row = width;
  const float4 start_slopes = texels[row + 0];
  const float4 end_slopes = texels[row + 1];
  const float4 range_minimums = texels[row + 2];
  const float4 range_dividers = texels[row + 3];

  /* First the combined curve, its alpha component, on all three channels; then each channel's own
   * curve. This is the RGB Curves node's `curves_combined_rgb`, the function the CPU applies. */
  for (const int channel : IndexRange(3)) {
    const float parameter = (rgb[channel] - range_minimums[3]) * range_dividers[3];
    float combined = curves_lut_component(texels, width, parameter, 3);
    combined = curves_lut_extrapolate(parameter, combined, start_slopes[3], end_slopes[3]);

    const float channel_parameter = (combined - range_minimums[channel]) *
                                    range_dividers[channel];
    float value = curves_lut_component(texels, width, channel_parameter, channel);
    rgb[channel] = curves_lut_extrapolate(
        channel_parameter, value, start_slopes[channel], end_slopes[channel]);
  }
}

void paint_layer_color_ramp_lut_build(const ColorBand &band,
                                      Vector<float4> &r_texels,
                                      int &r_width)
{
  /* 2048 samples rather than 1024: an HSV or HSL band is not linear in RGB, and a pair of close
   * stops can bend it sharply; two thousand samples keep the linear interpolation between them
   * inside the parity tolerance without a measurable cost (32 KiB). */
  r_width = 2048;
  r_texels.resize(r_width);
  for (const int i : IndexRange(r_width)) {
    const float value = float(i) / float(r_width - 1);
    float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    BKE_colorband_evaluate(&band, value, rgba);
    r_texels[i] = float4(rgba[0], rgba[1], rgba[2], rgba[3]);
  }
}

float paint_layer_color_ramp_lut_eval(const Span<float4> texels,
                                      const int width,
                                      const bool constant,
                                      const bool use_alpha,
                                      float rgb[3])
{
  /* The RGB to BW node's scene-linear Rec.709 luminance, as `paint_layer_color_ramp_apply`. */
  const float value = clamp_f(
      0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2], 0.0f, 1.0f);
  const float index = value * float(width - 1);
  float4 ramp;
  if (constant) {
    /* A CONSTANT band steps at its stops: the nearest sample is that step, no blend across it. */
    const int nearest = std::clamp(int(index + 0.5f), 0, width - 1);
    ramp = texels[nearest];
  }
  else {
    const float low = std::floor(index);
    const int i0 = std::clamp(int(low), 0, width - 1);
    const int i1 = std::clamp(int(low) + 1, 0, width - 1);
    ramp = texels[i0] + (texels[i1] - texels[i0]) * (index - low);
  }
  copy_v3_v3(rgb, ramp);
  return use_alpha ? clamp_f(ramp.w, 0.0f, 1.0f) : 1.0f;
}

uint64_t paint_layer_curves_hash(const CurveMapping &curve_mapping)
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
  /* What the evaluation reads: the clip and extrapolation flags, the levels and the tone, then
   * every curve's points. The preset and the current-curve index are UI state. */
  mix_u32(uint32_t(curve_mapping.flag));
  mix_u32(uint32_t(curve_mapping.tone));
  for (const int i : IndexRange(3)) {
    mix_float(curve_mapping.black[i]);
    mix_float(curve_mapping.white[i]);
  }
  for (const CurveMap &curve : curve_mapping.cm) {
    mix_u32(uint32_t(curve.totpoint));
    for (const int i : IndexRange(2)) {
      mix_float(curve.ext_in[i]);
      mix_float(curve.ext_out[i]);
    }
    for (const int i : IndexRange(curve.totpoint)) {
      mix_float(curve.curve[i].x);
      mix_float(curve.curve[i].y);
    }
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

bool paint_layer_kind_has_color_balance_param(const MaterialPaintLayer &row)
{
  const PaintLayerKindInfo *info = paint_layer_kind_of(row);
  if (info == nullptr) {
    return false;
  }
  for (const int i : IndexRange(info->params_num)) {
    if (info->params[i].type == PaintLayerKindParamType::ColorBalance) {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_kind_needs_bake(const MaterialPaintLayer &row)
{
  return paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_NEEDS_BAKE);
}

bool BKE_paint_layers_color_balance_supported(const MaterialPaintLayer &row)
{
  return paint_layer_kind_has_color_balance_param(row);
}

MaterialPaintLayerColorBalance *BKE_paint_layers_color_balance_ensure(MaterialPaintLayer &row)
{
  if (!paint_layer_kind_has_color_balance_param(row)) {
    return nullptr;
  }
  if (row.color_balance == nullptr) {
    /* The member initializers of #MaterialPaintLayerColorBalance are the neutral balance: every
     * method's identity (lift 0, gamma 1, gain 1; slope 1, offset 0, power 1). */
    row.color_balance = MEM_new<MaterialPaintLayerColorBalance>(__func__);
  }
  return row.color_balance;
}

const MaterialPaintLayerColorBalance &paint_layer_color_balance_or_neutral(
    const MaterialPaintLayer &row)
{
  static const MaterialPaintLayerColorBalance neutral = {};
  return (row.color_balance != nullptr) ? *row.color_balance : neutral;
}

void paint_layer_color_balance_copy(const MaterialPaintLayer &src, MaterialPaintLayer &dst)
{
  dst.color_balance = (src.color_balance != nullptr) ?
                          static_cast<MaterialPaintLayerColorBalance *>(
                              MEM_dupalloc(src.color_balance)) :
                          nullptr;
}

void paint_layer_color_balance_fold(const MaterialPaintLayerColorBalance &balance,
                                    float r_params[PAINT_LAYER_COLOR_BALANCE_FOLDED_NUM])
{
  /* Both methods fold into one form, `out_i = pow(max(c_i * A_i + B_i, 0), E_i)`, so the CPU
   * composite, the graph and the GPU read one layout; the triples land in the `.xyz` of the
   * float4s, which is why the fourths stay unused. A fold of the neutral payload is the identity:
   * A = 1, B = 0, E = 1. */
  if (eMaterialPaintLayerColorBalanceMethod(balance.method) ==
      MA_PAINT_LAYER_COLOR_BALANCE_ASC_CDL)
  {
    for (const int i : IndexRange(3)) {
      r_params[PAINT_LAYER_COLOR_BALANCE_A + i] = balance.slope[i];
      r_params[PAINT_LAYER_COLOR_BALANCE_B + i] = balance.offset[i];
      r_params[PAINT_LAYER_COLOR_BALANCE_E + i] = balance.power[i];
    }
    return;
  }
  /* LGG: `lifted = c (1 - lift) + lift`, then `max(lifted * gain, 0)` and the gamma's power. With
   * `A = (1 - lift) * gain` and `B = lift * gain` the linear part is `c * A + B`, the same form the
   * CDL folds into; the gamma is the power's inverse exponent. */
  for (const int i : IndexRange(3)) {
    r_params[PAINT_LAYER_COLOR_BALANCE_A + i] = (1.0f - balance.lift[i]) * balance.gain[i];
    r_params[PAINT_LAYER_COLOR_BALANCE_B + i] = balance.lift[i] * balance.gain[i];
    r_params[PAINT_LAYER_COLOR_BALANCE_E + i] = 1.0f / std::max(balance.gamma[i], 1e-6f);
  }
}

void paint_layer_color_balance_apply(const MaterialPaintLayerColorBalance &balance, float rgb[3])
{
  float params[PAINT_LAYER_COLOR_BALANCE_FOLDED_NUM];
  paint_layer_color_balance_fold(balance, params);
  for (const int i : IndexRange(3)) {
    /* The same channel formula the CPU composite's Color Balance case and the generated
     * MULTIPLY_ADD / MAXIMUM / POWER chain read. */
    rgb[i] = powf(std::max(rgb[i] * params[PAINT_LAYER_COLOR_BALANCE_A + i] +
                               params[PAINT_LAYER_COLOR_BALANCE_B + i],
                           0.0f),
                  params[PAINT_LAYER_COLOR_BALANCE_E + i]);
  }
}

uint64_t paint_layer_color_balance_hash(const MaterialPaintLayerColorBalance &balance)
{
  uint64_t hash = get_default_hash(uint32_t(balance.method));
  const auto mix_float = [&](const float value) {
    hash = get_default_hash(hash, value);
  };
  for (const float value : balance.lift) {
    mix_float(value);
  }
  for (const float value : balance.gamma) {
    mix_float(value);
  }
  for (const float value : balance.gain) {
    mix_float(value);
  }
  for (const float value : balance.slope) {
    mix_float(value);
  }
  for (const float value : balance.offset) {
    mix_float(value);
  }
  for (const float value : balance.power) {
    mix_float(value);
  }
  return hash;
}

bool BKE_paint_layers_color_balance_edited(Material &ma, const void *data)
{
  if (data == nullptr) {
    return false;
  }
  const auto owns = [&](const MaterialPaintLayer &row) {
    return row.color_balance != nullptr && data == row.color_balance;
  };
  bool found = false;
  for (MaterialPaintLayer &row : ma.paint_layers) {
    if (paint_layer_payload_edited_walk(row, owns)) {
      found = true;
      break;
    }
  }
  if (found) {
    /* The balance reaches the generated graph as the values of three group inputs and the
     * composite stack's hash carries the folded numbers, so the value-only tag is the whole
     * story: no group is rebuilt, and the bake flags follow the hash. */
    BKE_paint_layers_tag_value_edited(ma);
  }
  return found;
}

}  // namespace blender
