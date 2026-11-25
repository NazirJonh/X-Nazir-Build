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
#include "DNA_material_types.h"

#include "BKE_colorband.hh"
#include "BKE_colortools.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_material_composite.hh"

#include "BLO_read_write.hh"

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
    /* The vibrance lifts the muted colours away from their grey; zero leaves the filter as it was. */
    {"vibrance", "Vibrance", 0.0f, -1.0f, 1.0f},
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

static const PaintLayerKindParam invert_params[] = {
    /* One turns the channel all the way to its complement, so the defaults keep the invert the
     * unparameterised filter always was: an old material reads the same without a stored row. */
    {"amount_r", "Amount R", 1.0f, 0.0f, 1.0f},
    {"amount_g", "Amount G", 1.0f, 0.0f, 1.0f},
    {"amount_b", "Amount B", 1.0f, 0.0f, 1.0f},
};
static_assert(std::size(invert_params) == PAINT_LAYER_INVERT_PARAM_NUM);

static const PaintLayerKindEnumItem deband_mode_items[] = {
    {PAINT_LAYER_DEBAND_DEBAND, "DEBAND", "Deband"},
    {PAINT_LAYER_DEBAND_DITHER, "DITHER", "Dither"},
};
static_assert(int(std::size(deband_mode_items)) - 1 == PAINT_LAYER_DEBAND_DITHER);

static const PaintLayerKindParam deband_params[] = {
    /* The mode is chosen when the graph is built, so an edit of this is a shape edit; the noise
     * the two modes share stays values beside it. */
    {"mode",
     "Mode",
     float(PAINT_LAYER_DEBAND_DEBAND),
     0.0f,
     float(PAINT_LAYER_DEBAND_DITHER),
     PaintLayerKindParamType::Enum,
     deband_mode_items,
     int(std::size(deband_mode_items))},
    /* Whole bits: eight is where a byte image sits, sixteen leaves the colour alone. */
    {"bits", "Bits", 8.0f, 4.0f, 16.0f, PaintLayerKindParamType::Int},
    /* Whole seeds: which pattern of the family the texels read. */
    {"seed", "Seed", 0.0f, 0.0f, 65535.0f, PaintLayerKindParamType::Int},
};
static_assert(std::size(deband_params) == PAINT_LAYER_DEBAND_PARAM_NUM);

static const PaintLayerKindEnumItem noise_type_items[] = {
    {PAINT_LAYER_NOISE_GAUSSIAN, "GAUSSIAN", "Gaussian"},
    {PAINT_LAYER_NOISE_UNIFORM, "UNIFORM", "Uniform"},
    {PAINT_LAYER_NOISE_SPECKLE, "SPECKLE", "Speckle"},
    {PAINT_LAYER_NOISE_SALT_PEPPER, "SALT_PEPPER", "Salt & Pepper"},
};
static_assert(int(std::size(noise_type_items)) - 1 == PAINT_LAYER_NOISE_SALT_PEPPER);

static const PaintLayerKindParam noise_params[] = {
    /* The family is chosen when the effect is computed; the amounts the families share stay values
     * beside it. */
    {"type",
     "Type",
     float(PAINT_LAYER_NOISE_GAUSSIAN),
     0.0f,
     float(PAINT_LAYER_NOISE_SALT_PEPPER),
     PaintLayerKindParamType::Enum,
     noise_type_items,
     int(std::size(noise_type_items))},
    /* A sigma in colour: half keeps the grain heavy, zero lays none at all. */
    {"intensity", "Intensity", 0.05f, 0.0f, 0.5f},
    /* Through the luminance or over the colour, as the addon's noise_color starts. A checkbox. */
    {"monochrome", "Monochrome", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Bool},
    /* Whole seeds: which pattern of the family the texels read. */
    {"seed", "Seed", 0.0f, 0.0f, 65535.0f, PaintLayerKindParamType::Int},
};
static_assert(std::size(noise_params) == PAINT_LAYER_NOISE_PARAM_NUM);

static const PaintLayerKindEnumItem normal_convert_mode_items[] = {
    {PAINT_LAYER_NORMAL_CONVERT_DX_TO_GL, "DX_TO_GL", "DirectX to OpenGL"},
    {PAINT_LAYER_NORMAL_CONVERT_GL_TO_DX, "GL_TO_DX", "OpenGL to DirectX"},
    {PAINT_LAYER_NORMAL_CONVERT_REBUILD_Z, "REBUILD_Z", "Rebuild Z"},
    {PAINT_LAYER_NORMAL_CONVERT_CLEAR_Z, "CLEAR_Z", "Clear Z"},
    {PAINT_LAYER_NORMAL_CONVERT_LINEAR_TO_SRGB, "LINEAR_TO_SRGB", "Linear to sRGB"},
    {PAINT_LAYER_NORMAL_CONVERT_SRGB_TO_LINEAR, "SRGB_TO_LINEAR", "sRGB to Linear"},
};
static_assert(int(std::size(normal_convert_mode_items)) - 1 ==
              PAINT_LAYER_NORMAL_CONVERT_SRGB_TO_LINEAR);

static const PaintLayerKindEnumItem normal_convert_normalize_items[] = {
    {0, "OFF", "Off"},
    {1, "ON", "On"},
};
static_assert(int(std::size(normal_convert_normalize_items)) - 1 == 1);

static const PaintLayerKindParam normal_convert_params[] = {
    /* The mode is chosen when the graph is built, so an edit of this is a shape edit; so is the
     * normalize, which adds nodes to the chain. */
    {"mode",
     "Mode",
     float(PAINT_LAYER_NORMAL_CONVERT_DX_TO_GL),
     0.0f,
     float(PAINT_LAYER_NORMAL_CONVERT_SRGB_TO_LINEAR),
     PaintLayerKindParamType::Enum,
     normal_convert_mode_items,
     int(std::size(normal_convert_mode_items))},
    {"normalize",
     "Normalize",
     1.0f,
     0.0f,
     1.0f,
     PaintLayerKindParamType::Enum,
     normal_convert_normalize_items,
     int(std::size(normal_convert_normalize_items))},
};
static_assert(std::size(normal_convert_params) == PAINT_LAYER_NORMAL_CONVERT_PARAM_NUM);

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

static const PaintLayerKindParam histogram_scan_params[] = {
    /* The crossover point of the remap: the brightness the ramp walks from zero to one around.
     * Half, the Threshold's own default, keeps the midtones where they are. */
    {"position", "Position", 0.5f, 0.0f, 1.0f},
    /* The half-width of the transition each side of the position; zero is the hard edge the
     * Threshold alone makes, a full one is the ramp a value of the position's width walks. */
    {"contrast", "Contrast", 0.0f, 0.0f, 1.0f},
    /* The two states flip: what the position grew stays what it shrinks. */
    {"invert_position", "Invert Position", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Bool},
};
static_assert(std::size(histogram_scan_params) == PAINT_LAYER_HISTOGRAM_SCAN_PARAM_NUM);

static const PaintLayerKindParam histogram_shift_params[] = {
    /* The fraction of the tonal range the value rolls by, cyclic: a full turn is the value
     * again, so the slider's ends read as the same image. */
    {"position", "Position", 0.0f, 0.0f, 1.0f},
};
static_assert(std::size(histogram_shift_params) == PAINT_LAYER_HISTOGRAM_SHIFT_PARAM_NUM);

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

static const PaintLayerKindParam blur_directional_params[] = {
    /* Degrees, counter-clockwise off the map's +X: the direction the streaks run along. */
    {"angle", "Angle", 0.0f, 0.0f, 360.0f},
    /* Whole texels: the line's half reach, the distance the average walks each way. */
    {"radius", "Radius", 8.0f, 1.0f, 64.0f, PaintLayerKindParamType::Int},
    /* How much of the line average the result takes: zero is the colour as it is, one is all
     * average, and the values between soften the streak into the colour. */
    {"mix", "Intensity", 1.0f, 0.0f, 1.0f},
};
static_assert(std::size(blur_directional_params) == PAINT_LAYER_BLUR_DIRECTIONAL_PARAM_NUM);

static const PaintLayerKindEnumItem drop_shadow_source_items[] = {
    {PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA, "ALPHA", "Alpha"},
    {PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE, "LUMINANCE", "Luminance"},
};
static_assert(int(std::size(drop_shadow_source_items)) - 1 ==
              PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE);

static const PaintLayerKindParam drop_shadow_params[] = {
    /* The shape that casts: a paint stroke's coverage, or the bright side of the content. A mask
     * item ignores this and always reads the luminance, for the grey the mask lays carries no alpha
     * of its own. A channel that keeps no content alpha has none to cast from either, so an alpha
     * source lays no shadow there. */
    {"source",
     "Source",
     float(PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA),
     0.0f,
     float(PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE),
     PaintLayerKindParamType::Enum,
     drop_shadow_source_items,
     int(std::size(drop_shadow_source_items))},
    /* Degrees, counter-clockwise off the map's +X: the direction the shadow is cast toward. */
    {"angle", "Angle", 135.0f, 0.0f, 360.0f},
    /* Whole texels the silhouette is offset by, along the angle. */
    {"distance", "Distance", 4.0f, 0.0f, 64.0f, PaintLayerKindParamType::Int},
    /* Whole texels the box blur of the shadow edge reaches; zero leaves a hard edge. */
    {"size", "Size", 8.0f, 0.0f, 64.0f, PaintLayerKindParamType::Int},
    /* Whole texels the silhouette grows (a positive spread) or shrinks (a negative one) before
     * the blur, the erosion the spread of a cast shadow makes of the shape's edge. */
    {"spread", "Spread", 0.0f, -64.0f, 64.0f, PaintLayerKindParamType::Int},
    {"opacity", "Opacity", 1.0f, 0.0f, 1.0f},
    {"color",
     "Color",
     0.0f,
     0.0f,
     1.0f,
     PaintLayerKindParamType::Color,
     nullptr,
     0,
     {0.0f, 0.0f, 0.0f, 1.0f}},
};
static_assert(std::size(drop_shadow_params) == PAINT_LAYER_DROP_SHADOW_PARAM_NUM);

static const PaintLayerKindEnumItem mask_outline_position_items[] = {
    {PAINT_LAYER_MASK_OUTLINE_OUTSIDE, "OUTSIDE", "Outside"},
    {PAINT_LAYER_MASK_OUTLINE_INSIDE, "INSIDE", "Inside"},
    {PAINT_LAYER_MASK_OUTLINE_DOUBLE, "DOUBLE", "Double"},
};
static_assert(int(std::size(mask_outline_position_items)) - 1 ==
              PAINT_LAYER_MASK_OUTLINE_DOUBLE);

static const PaintLayerKindParam mask_outline_params[] = {
    {"position",
     "Mask Position",
     float(PAINT_LAYER_MASK_OUTLINE_OUTSIDE),
     0.0f,
     float(PAINT_LAYER_MASK_OUTLINE_DOUBLE),
     PaintLayerKindParamType::Enum,
     mask_outline_position_items,
     int(std::size(mask_outline_position_items))},
    /* On: the dark side of the threshold is the inside, the way an inverted mask item reads. */
    {"start_from_black", "Start From Black", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Bool},
    /* The grey the border of the shape is read at: what crosses it is in, what falls short is
     * out. */
    {"threshold", "Threshold", 0.5f, 0.0f, 1.0f},
    /* Whole texels the band takes to fall from the border to zero. */
    {"width", "Width", 4.0f, 1.0f, 64.0f, PaintLayerKindParamType::Int},
    /* Whole texels of the box blur laid over the finished band. */
    {"blur", "Blur", 0.0f, 0.0f, 32.0f, PaintLayerKindParamType::Int},
    /* The falloff's bend from a line to the smoothstep: zero is the line, one the smoothstep's
     * own curve. */
    {"curve_shape", "Curve Shape", 0.5f, 0.0f, 1.0f},
    /* The shape a content row's ring is laid around: its alpha (a stroke's coverage) or the
     * luminance. A mask item ignores this and always reads the luminance of its grey. */
    {"source",
     "Source",
     float(PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA),
     0.0f,
     float(PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE),
     PaintLayerKindParamType::Enum,
     drop_shadow_source_items,
     int(std::size(drop_shadow_source_items))},
    /* The colour of the ring on a content row; a mask lays a grey band instead. */
    {"color",
     "Color",
     0.0f,
     0.0f,
     1.0f,
     PaintLayerKindParamType::Color,
     nullptr,
     0,
     {1.0f, 1.0f, 1.0f, 1.0f}},
};
static_assert(std::size(mask_outline_params) == PAINT_LAYER_MASK_OUTLINE_PARAM_NUM);

static const PaintLayerKindEnumItem fill_area_source_items[] = {
    {PAINT_LAYER_FILL_AREA_SOURCE_ALPHA, "ALPHA", "Alpha"},
    {PAINT_LAYER_FILL_AREA_SOURCE_LUMINANCE, "LUMINANCE", "Luminance"},
};
static_assert(int(std::size(fill_area_source_items)) - 1 ==
              PAINT_LAYER_FILL_AREA_SOURCE_LUMINANCE);

static const PaintLayerKindEnumItem fill_area_uv_border_items[] = {
    {PAINT_LAYER_FILL_AREA_UV_BORDER_FILL, "FILL", "Fill"},
    {PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE, "IGNORE", "Ignore"},
};
static_assert(int(std::size(fill_area_uv_border_items)) - 1 ==
              PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE);

static const PaintLayerKindEnumItem fill_area_color_mode_items[] = {
    {PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE, "SOURCE", "Source"},
    {PAINT_LAYER_FILL_AREA_COLOR_MODE_SOLID, "SOLID", "Solid"},
};
static_assert(int(std::size(fill_area_color_mode_items)) - 1 ==
              PAINT_LAYER_FILL_AREA_COLOR_MODE_SOLID);

static const PaintLayerKindParam fill_area_params[] = {
    /* The border the fill closes against: a stroke's coverage, or the dark side of the content --
     * a mask item reads the latter, for the grey the mask lays carries no alpha of its own. */
    {"source",
     "Area Detection",
     float(PAINT_LAYER_FILL_AREA_SOURCE_ALPHA),
     0.0f,
     float(PAINT_LAYER_FILL_AREA_SOURCE_LUMINANCE),
     PaintLayerKindParamType::Enum,
     fill_area_source_items,
     int(std::size(fill_area_source_items))},
    /* The value the border is read at: what crosses it is border, what falls short is region. */
    {"threshold", "Area Detection Threshold", 0.5f, 0.0f, 1.0f},
    /* The regions themselves, as a grey ramp of their number: the check that finds the strokes
     * whose border is not closed. */
    {"debug", "Debug Area Detection", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Bool},
    /* What a region open at the map's border does: the border closes it, or it stays open. */
    {"uv_border",
     "UV Border Behavior",
     float(PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE),
     0.0f,
     float(PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE),
     PaintLayerKindParamType::Enum,
     fill_area_uv_border_items,
     int(std::size(fill_area_uv_border_items))},
    /* What the enclosed region takes: the border's own colours, or the solid one. */
    {"color_mode",
     "Color Mode",
     float(PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE),
     0.0f,
     float(PAINT_LAYER_FILL_AREA_COLOR_MODE_SOLID),
     PaintLayerKindParamType::Enum,
     fill_area_color_mode_items,
     int(std::size(fill_area_color_mode_items))},
    {"color",
     "Fill Color",
     0.0f,
     0.0f,
     1.0f,
     PaintLayerKindParamType::Color,
     nullptr,
     0,
     {1.0f, 1.0f, 1.0f, 1.0f}},
    /* Whole texels of the box blur laid over the finished fill. */
    {"blur", "Blur Intensity", 0.0f, 0.0f, 32.0f, PaintLayerKindParamType::Int},
    /* The diffusion passes: the useful range the filter documents is 8 to 48; more reach farther
     * into a wide region, at the cost of the passes. */
    {"iterations", "Diffusion Iterations", 24.0f, 0.0f, 64.0f, PaintLayerKindParamType::Int},
};
static_assert(std::size(fill_area_params) == PAINT_LAYER_FILL_AREA_PARAM_NUM);

static const PaintLayerKindEnumItem channel_split_channel_items[] = {
    {PAINT_LAYER_CHANNEL_SPLIT_RED, "RED", "Red"},
    {PAINT_LAYER_CHANNEL_SPLIT_GREEN, "GREEN", "Green"},
    {PAINT_LAYER_CHANNEL_SPLIT_BLUE, "BLUE", "Blue"},
    {PAINT_LAYER_CHANNEL_SPLIT_ALPHA, "ALPHA", "Alpha"},
};
static_assert(int(std::size(channel_split_channel_items)) - 1 ==
              PAINT_LAYER_CHANNEL_SPLIT_ALPHA);

static const PaintLayerKindParam channel_split_params[] = {
    /* The channel of the slot's image the split extracts: an ORM map's red is the occlusion, its
     * green the roughness and its blue the metallic, so one effect per target reads one channel. */
    {"channel",
     "Source Channel",
     float(PAINT_LAYER_CHANNEL_SPLIT_RED),
     0.0f,
     float(PAINT_LAYER_CHANNEL_SPLIT_ALPHA),
     PaintLayerKindParamType::Enum,
     channel_split_channel_items,
     int(std::size(channel_split_channel_items))},
};
static_assert(std::size(channel_split_params) == PAINT_LAYER_CHANNEL_SPLIT_PARAM_NUM);

static const PaintLayerKindEnumItem normal_to_mask_mode_items[] = {
    {PAINT_LAYER_NORMAL_TO_MASK_HEIGHT, "HEIGHT", "Height"},
    {PAINT_LAYER_NORMAL_TO_MASK_EDGES, "EDGES", "Edges"},
    {PAINT_LAYER_NORMAL_TO_MASK_SLOPE, "SLOPE", "Slope"},
};
static_assert(int(std::size(normal_to_mask_mode_items)) - 1 == PAINT_LAYER_NORMAL_TO_MASK_SLOPE);

static const PaintLayerKindParam normal_to_mask_params[] = {
    /* The field the normal rebuilds into: the kernel is chosen when the effect is computed, so
     * an edit of this is a shape edit and the caches rebuild, the way the Blur's shape is. */
    {"mode",
     "Mode",
     float(PAINT_LAYER_NORMAL_TO_MASK_HEIGHT),
     0.0f,
     float(PAINT_LAYER_NORMAL_TO_MASK_SLOPE),
     PaintLayerKindParamType::Enum,
     normal_to_mask_mode_items,
     int(std::size(normal_to_mask_mode_items))},
    /* The position the cut centers on, in the field's normalized [0, 1]: the mask grows and
     * shrinks by eating the dark side away. */
    {"threshold", "Threshold", 0.5f, 0.0f, 1.0f},
    /* The ramp's width around the position: zero is the hard cut the Threshold makes, one a ramp
     * the whole range walks. */
    {"softness", "Softness", 0.1f, 0.0f, 1.0f},
};
static_assert(std::size(normal_to_mask_params) == PAINT_LAYER_NORMAL_TO_MASK_PARAM_NUM);

static const PaintLayerKindParam height_to_normal_params[] = {
    /* The gradients' multiplier: one reads as the Sobel's own scale, two doubles the slope. */
    {"strength", "Normal Intensity", 1.0f, 0.0f, 16.0f},
    /* The gradients scale by the physical size instead of the map's texels: the same surface laid
     * at twice the resolution keeps the same normal. */
    {"world_units", "Use World Units", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Bool},
    /* Centimetres the map's width (or height, whichever is laid wider) covers. */
    {"surface_size", "Surface Size (cm)", 100.0f, 0.0f, 1000.0f},
    /* Centimetres the height delta covers, from the lowest texel to the highest. */
    {"height_depth", "Height Depth (cm)", 5.0f, 0.0f, 100.0f},
    /* The green flips, the DirectX encoding: the project's own setting is the OpenGL one. */
    {"flip_green", "Flip Green", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Bool},
};
static_assert(std::size(height_to_normal_params) == PAINT_LAYER_HEIGHT_TO_NORMAL_PARAM_NUM);

static const PaintLayerKindEnumItem hsl_perspective_mode_items[] = {
    {PAINT_LAYER_HSL_PERSPECTIVE_REGULAR, "REGULAR", "Regular"},
    {PAINT_LAYER_HSL_PERSPECTIVE_PERCEPTIVE, "PERCEPTIVE", "Perceptive"},
};
static_assert(int(std::size(hsl_perspective_mode_items)) - 1 ==
              PAINT_LAYER_HSL_PERSPECTIVE_PERCEPTIVE);

static const PaintLayerKindEnumItem slope_source_items[] = {
    {PAINT_LAYER_SLOPE_SOURCE_NOISE, "DEFAULT_NOISE", "Default Noise"},
    {PAINT_LAYER_SLOPE_SOURCE_PREVIOUS_INPUT, "PREVIOUS_INPUT", "Previous Input"},
};
static_assert(int(std::size(slope_source_items)) - 1 == PAINT_LAYER_SLOPE_SOURCE_PREVIOUS_INPUT);

static const PaintLayerKindEnumItem blur_slope_blending_items[] = {
    {PAINT_LAYER_BLUR_SLOPE_BLENDING_BLUR, "BLUR", "Blur"},
    {PAINT_LAYER_BLUR_SLOPE_BLENDING_MIN, "MIN", "Min"},
    {PAINT_LAYER_BLUR_SLOPE_BLENDING_MAX, "MAX", "Max"},
};
static_assert(int(std::size(blur_slope_blending_items)) - 1 ==
              PAINT_LAYER_BLUR_SLOPE_BLENDING_MAX);

static const PaintLayerKindParam smoothstep_params[] = {
    /* The low edge of the extracted range: what falls short of it reads zero. */
    {"minimum", "Minimum", 0.0f, 0.0f, 1.0f},
    /* The high edge: what passes it reads one. */
    {"maximum", "Maximum", 1.0f, 0.0f, 1.0f},
};
static_assert(std::size(smoothstep_params) == PAINT_LAYER_SMOOTHSTEP_PARAM_NUM);

static const PaintLayerKindParam contrast_luminosity_params[] = {
    /* The distance from the middle scales: a negative contrast folds it in, a positive one
     * spreads it. */
    {"contrast", "Contrast", 0.0f, -1.0f, 1.0f},
    /* The shift every channel takes, after the contrast has scaled. */
    {"luminosity", "Luminosity", 0.0f, -1.0f, 1.0f},
};
static_assert(std::size(contrast_luminosity_params) == PAINT_LAYER_CONTRAST_LUMINOSITY_PARAM_NUM);

static const PaintLayerKindParam height_adjust_params[] = {
    /* On: the value flips before the offset reads it. */
    {"invert", "Invert", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Bool},
    /* The offset rides before the multiply, the order the filter's own note pins: a zero offset
     * result annihilates the multiply at that point. */
    {"offset", "Offset", 0.0f, -1.0f, 1.0f},
    /* The multiplier: zero leaves the value at zero, two doubles the relief. */
    {"multiply", "Multiply", 1.0f, 0.0f, 4.0f},
};
static_assert(std::size(height_adjust_params) == PAINT_LAYER_HEIGHT_ADJUST_PARAM_NUM);

static const PaintLayerKindParam pixelate_params[] = {
    /* The pixel count the map cuts into per side: more pixels, smaller blocks, finer detail. */
    {"amount", "Pixels Amount", 64.0f, 1.0f, 256.0f, PaintLayerKindParamType::Int},
};
static_assert(std::size(pixelate_params) == PAINT_LAYER_PIXELATE_PARAM_NUM);

static const PaintLayerKindParam hsl_perspective_params[] = {
    {"mode",
     "Mode",
     float(PAINT_LAYER_HSL_PERSPECTIVE_REGULAR),
     0.0f,
     float(PAINT_LAYER_HSL_PERSPECTIVE_PERCEPTIVE),
     PaintLayerKindParamType::Enum,
     hsl_perspective_mode_items,
     int(std::size(hsl_perspective_mode_items))},
    /* Bipolar around zero: the full sweep shifts the hue half a turn each way, the same ±180
     * degrees the slider reads. */
    {"hue", "Hue", 0.0f, -0.5f, 0.5f},
    /* Bipolar around zero: the factor the saturation carries is one plus the double, so the full
     * sweep drains it or doubles it. */
    {"saturation", "Saturation", 0.0f, -0.5f, 0.5f},
    /* Bipolar around zero: the Regular mode scales the value channel by the same factor, the
     * Perceptive one lifts toward white or black. */
    {"lightness", "Lightness", 0.0f, -0.5f, 0.5f},
};
static_assert(std::size(hsl_perspective_params) == PAINT_LAYER_HSL_PERSPECTIVE_PARAM_NUM);

static const PaintLayerKindParam blur_slope_params[] = {
    {"seed", "Seed", 0.0f, 0.0f, 10000.0f, PaintLayerKindParamType::Int},
    /* Texels the walk spans at most, along the slope. */
    {"intensity", "Intensity", 15.0f, 0.0f, 16.0f},
    /* The samples the walk takes: the quality, at the cost of the speed. */
    {"quality", "Quality", 8.0f, 0.0f, 32.0f, PaintLayerKindParamType::Int},
    {"blending",
     "Blending Mode",
     float(PAINT_LAYER_BLUR_SLOPE_BLENDING_BLUR),
     0.0f,
     float(PAINT_LAYER_BLUR_SLOPE_BLENDING_MAX),
     PaintLayerKindParamType::Enum,
     blur_slope_blending_items,
     int(std::size(blur_slope_blending_items))},
    {"source_mode",
     "Source Type",
     float(PAINT_LAYER_SLOPE_SOURCE_NOISE),
     0.0f,
     float(PAINT_LAYER_SLOPE_SOURCE_PREVIOUS_INPUT),
     PaintLayerKindParamType::Enum,
     slope_source_items,
     int(std::size(slope_source_items))},
    /* Whole texels the source is blurred by before it drives the slope. */
    {"source_blur", "Blur", 0.0f, 0.0f, 32.0f, PaintLayerKindParamType::Int},
    /* The midpoint the source shifts by, like a brightness. */
    {"source_position", "Position", 0.0f, -1.0f, 1.0f},
    /* The contrast the source carries: one is the source as it is. */
    {"source_contrast", "Contrast", 1.0f, 0.0f, 2.0f},
    /* The tiles the noise covers the map with. */
    {"source_tilling", "Source Tilling", 1.0f, 0.25f, 16.0f},
};
static_assert(std::size(blur_slope_params) == PAINT_LAYER_BLUR_SLOPE_PARAM_NUM);

static const PaintLayerKindEnumItem glow_mode_items[] = {
    {PAINT_LAYER_GLOW_SOFT, "SOFT", "Soft"},
    {PAINT_LAYER_GLOW_PRECISE, "PRECISE", "Precise"},
};
static_assert(int(std::size(glow_mode_items)) - 1 == PAINT_LAYER_GLOW_PRECISE);

static const PaintLayerKindParam glow_params[] = {
    {"mode",
     "Mode",
     float(PAINT_LAYER_GLOW_SOFT),
     0.0f,
     float(PAINT_LAYER_GLOW_PRECISE),
     PaintLayerKindParamType::Enum,
     glow_mode_items,
     int(std::size(glow_mode_items))},
    {"width", "Width", 8.0f, 1.0f, 64.0f, PaintLayerKindParamType::Int},
    /* The source luminance is clamped to [0, 1], so at a spread of one the mask would be zero
     * everywhere: the slider stops one step below, where the mask is the narrow, sharp remnant of
     * the brightest texels. */
    {"spread", "Spread", 0.0f, 0.0f, 0.99f},
    {"opacity", "Opacity", 1.0f, 0.0f, 1.0f},
    {"color",
     "Color",
     0.0f,
     0.0f,
     1.0f,
     PaintLayerKindParamType::Color,
     nullptr,
     0,
     {1.0f, 1.0f, 1.0f, 1.0f}},
};
static_assert(std::size(glow_params) == PAINT_LAYER_GLOW_PARAM_NUM);

static const PaintLayerKindEnumItem bevel_corner_items[] = {
    {PAINT_LAYER_BEVEL_ROUND, "ROUND", "Round"},
    {PAINT_LAYER_BEVEL_ANGULAR, "ANGULAR", "Angular"},
};
static_assert(int(std::size(bevel_corner_items)) - 1 == PAINT_LAYER_BEVEL_ANGULAR);

static const PaintLayerKindEnumItem bevel_source_items[] = {
    {PAINT_LAYER_BEVEL_SOURCE_ALPHA, "ALPHA", "Alpha"},
    {PAINT_LAYER_BEVEL_SOURCE_LUMINANCE, "LUMINANCE", "Luminance"},
};
static_assert(int(std::size(bevel_source_items)) - 1 == PAINT_LAYER_BEVEL_SOURCE_LUMINANCE);

static const PaintLayerKindParam bevel_params[] = {
    {"source",
     "Source",
     float(PAINT_LAYER_BEVEL_SOURCE_ALPHA),
     0.0f,
     float(PAINT_LAYER_BEVEL_SOURCE_LUMINANCE),
     PaintLayerKindParamType::Enum,
     bevel_source_items,
     int(std::size(bevel_source_items))},
    {"distance", "Distance", 4.0f, 1.0f, 64.0f, PaintLayerKindParamType::Int},
    {"corner",
     "Corner Type",
     float(PAINT_LAYER_BEVEL_ROUND),
     0.0f,
     float(PAINT_LAYER_BEVEL_ANGULAR),
     PaintLayerKindParamType::Enum,
     bevel_corner_items,
     int(std::size(bevel_corner_items))},
    {"smooth", "Smooth Blur", 0.0f, 0.0f, 64.0f, PaintLayerKindParamType::Int},
};
static_assert(std::size(bevel_params) == PAINT_LAYER_BEVEL_PARAM_NUM);

static const PaintLayerKindParam color_match_params[] = {
    /* Checkboxes, not enums: a choice that only picks which value the kernel reads is a value edit,
     * not a shape edit. */
    {"source_picked", "Picked Source", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Bool},
    {"source_color",
     "Source",
     0.0f,
     0.0f,
     1.0f,
     PaintLayerKindParamType::Color,
     nullptr,
     0,
     {0.5f, 0.5f, 0.5f, 1.0f}},
    {"target_color",
     "Target",
     0.0f,
     0.0f,
     1.0f,
     PaintLayerKindParamType::Color,
     nullptr,
     0,
     {0.5f, 0.5f, 0.5f, 1.0f}},
    {"variation", "Color Variation", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Bool},
    /* Turns of hue: half a turn is the whole circle either way. */
    {"variation_hue", "Hue", 0.0f, 0.0f, 0.5f},
    {"variation_chroma", "Chroma", 0.0f, 0.0f, 1.0f},
    {"variation_luma", "Luma", 0.0f, 0.0f, 1.0f},
};
static_assert(std::size(color_match_params) == PAINT_LAYER_COLOR_MATCH_PARAM_NUM);

static const PaintLayerKindParam exposure_params[] = {
    /* Stops of light: one unit doubles the colour, so it reads as an exposure compensation. */
    {"exposure", "Exposure", 0.0f, -5.0f, 5.0f},
    /* Never zero, and one leaves a colour above one alone: the power is one over it. */
    {"gamma", "Gamma", 1.0f, 0.1f, 9.99f},
};
static_assert(std::size(exposure_params) == PAINT_LAYER_EXPOSURE_PARAM_NUM);

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

/* The matrix lives in #MaterialPaintLayer::channel_mixer; the schema entry is what tells the row
 * its kind has one. The monochrome switch is a plain Bool of the schema: it is folded into the 12
 * numbers (#paint_layer_channel_mixer_fold) the graph and the GPU read, so it is a value edit like
 * the matrix and is deliberately not an Enum, which would reach the topology hash and rebuild a
 * tree whose shape the fold has already settled. */
static const PaintLayerKindParam channel_mixer_params[] = {
    {"mixer", "Channel Mixer", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::ChannelMixer},
    {"monochrome", "Monochrome", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Bool},
};
static_assert(std::size(channel_mixer_params) == PAINT_LAYER_CHANNEL_MIXER_PARAM_NUM);

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

float paint_layer_deband_noise(int x, int y, int seed, int channel, int pass)
{
  /* Two passes per channel: the salt keeps them and the three channels apart. */
  return paint_layer_texel_hash_uniform(x, y, seed, uint32_t(channel * 2 + pass));
}

void paint_layer_color_match_apply(const float source_color[3],
                                   const float target_color[3],
                                   const PaintLayerColorMatchVariation &variation,
                                   const int x,
                                   const int y,
                                   float rgb[3])
{
  /* The grade in HSL: each component of the texel moves by the target's difference from the
   * source's, so a texel of the source's colour lands exactly on the target. */
  float source_hsl[3], target_hsl[3], texel_hsl[3];
  rgb_to_hsl_v(source_color, source_hsl);
  rgb_to_hsl_v(target_color, target_hsl);
  rgb_to_hsl_v(rgb, texel_hsl);

  float hue = texel_hsl[0] + (target_hsl[0] - source_hsl[0]);
  hue -= floorf(hue);
  float chroma = clamp_f(texel_hsl[1] + (target_hsl[1] - source_hsl[1]), 0.0f, 1.0f);
  float luma = clamp_f(texel_hsl[2] + (target_hsl[2] - source_hsl[2]), 0.0f, 1.0f);

  if (variation.hue > 0.0f || variation.chroma > 0.0f || variation.luma > 0.0f) {
    /* A hash of the texel keeps the jitter stable between evaluations; the salts keep the three
     * components independent of each other. */
    const auto jitter = [&](const uint32_t salt) {
      return paint_layer_texel_hash_uniform(x, y, 0, salt) * 2.0f - 1.0f;
    };
    hue += jitter(1) * variation.hue;
    hue -= floorf(hue);
    chroma = clamp_f(chroma + jitter(2) * variation.chroma, 0.0f, 1.0f);
    luma = clamp_f(luma + jitter(3) * variation.luma, 0.0f, 1.0f);
  }

  const float hsl[3] = {hue, chroma, luma};
  hsl_to_rgb_v(hsl, rgb);
}

/**
 * The addon widens its two-uniform difference by this factor. A plain TPDF spans exactly one
 * quantization step; the extra half reaches across the neighbouring boundary often enough to break
 * the band up without raising the visible grain as much as a whole extra step would.
 */
static constexpr float PAINT_LAYER_DEBAND_NOISE_SCALE = 1.5f;

void paint_layer_deband_apply(float rgb[3],
                              const float params[PAINT_LAYER_DEBAND_PARAM_NUM],
                              const int texel_xy[2])
{
  /* The addon's TPDF: two independent uniforms make a triangular density of the step's own width,
   * one noise pair per channel and texel, as its per-pixel randoms are. */
  const int x = (texel_xy != nullptr) ? texel_xy[0] : 0;
  const int y = (texel_xy != nullptr) ? texel_xy[1] : 0;
  const int seed = int(params[PAINT_LAYER_DEBAND_SEED]);
  const int mode = int(params[PAINT_LAYER_DEBAND_MODE]);
  const int levels = (1 << int(params[PAINT_LAYER_DEBAND_BITS])) - 1;
  const float step = 1.0f / float(levels);
  for (int k = 0; k < 3; k++) {
    const float u1 = paint_layer_deband_noise(x, y, seed, k, 1);
    const float u2 = paint_layer_deband_noise(x, y, seed, k, 2);
    const float noise = (u1 - u2) * step;
    if (mode == PAINT_LAYER_DEBAND_DITHER) {
      rgb[k] = clamp_f(
          floorf(rgb[k] * float(levels) + (u1 - u2) + 0.5f) / float(levels), 0.0f, 1.0f);
    }
    else {
      rgb[k] = clamp_f(rgb[k] + noise * PAINT_LAYER_DEBAND_NOISE_SCALE, 0.0f, 1.0f);
    }
  }
}

void paint_layer_normal_convert_apply(float rgb[3],
                                      const float params[PAINT_LAYER_NORMAL_CONVERT_PARAM_NUM])
{
  const int mode = int(params[PAINT_LAYER_NORMAL_CONVERT_MODE]);
  switch (mode) {
    case PAINT_LAYER_NORMAL_CONVERT_DX_TO_GL:
    case PAINT_LAYER_NORMAL_CONVERT_GL_TO_DX:
      rgb[1] = 1.0f - rgb[1];
      break;
    case PAINT_LAYER_NORMAL_CONVERT_REBUILD_Z: {
      /* The two encoded xy, decoded; the blue the unit length leaves, encoded again. */
      const float vx = rgb[0] * 2.0f - 1.0f;
      const float vy = rgb[1] * 2.0f - 1.0f;
      const float z = sqrtf(std::max(1.0f - vx * vx - vy * vy, 0.0f));
      rgb[2] = (z + 1.0f) * 0.5f;
      break;
    }
    case PAINT_LAYER_NORMAL_CONVERT_CLEAR_Z:
      rgb[2] = 0.0f;
      break;
    case PAINT_LAYER_NORMAL_CONVERT_LINEAR_TO_SRGB:
      /* The IEC 61966-2.1 broken line, with the same constants the Math nodes of the graph carry. */
      for (int k = 0; k < 3; k++) {
        rgb[k] = (rgb[k] <= 0.0031308f) ? rgb[k] * 12.92f :
                                          1.055f * powf(rgb[k], 1.0f / 2.4f) - 0.055f;
      }
      break;
    case PAINT_LAYER_NORMAL_CONVERT_SRGB_TO_LINEAR:
      for (int k = 0; k < 3; k++) {
        rgb[k] = (rgb[k] <= 0.04045f) ? rgb[k] / 12.92f : powf((rgb[k] + 0.055f) / 1.055f, 2.4f);
      }
      break;
    default:
      break;
  }
  if (int(params[PAINT_LAYER_NORMAL_CONVERT_NORMALIZE]) != 0) {
    const float vx = rgb[0] * 2.0f - 1.0f;
    const float vy = rgb[1] * 2.0f - 1.0f;
    const float vz = rgb[2] * 2.0f - 1.0f;
    /* A zero vector has no direction: the floor keeps the division finite. */
    const float len = std::max(sqrtf(vx * vx + vy * vy + vz * vz), 1e-10f);
    rgb[0] = (vx / len) * 0.5f + 0.5f;
    rgb[1] = (vy / len) * 0.5f + 0.5f;
    rgb[2] = (vz / len) * 0.5f + 0.5f;
  }
}

/* The mask a laid kind takes, shared with the generator schemas
 * (#paint_layer_all_roles in paint_layers_kind.hh). */
static constexpr uint8_t all_roles = paint_layer_all_roles;

/* As a mask item a filter works on the mask's coverage, a single grey: it has no channels there. */
static constexpr uint8_t filter_roles = paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER) |
                                        paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT) |
                                        paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM);

/* A CPU-computed effect corrects a row's colour as an EFFECT, or the mask's coverage as a MASK
 * ITEM: the mask form reads the grey the mask has built up to it through its own cache, not the
 * row's, so the same kind is legal in both places and the place decides what its input is. */
static constexpr uint8_t effect_or_mask_roles = paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT) |
                                                paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM);

/* A transform effect exists as a content correction only, for now: its mask form would have to
 * resample the coverage the mask has built up to it, which the pointwise mask walk has no pass
 * for, so a Mirror in the mask waits for that pass rather than laying nothing silently. */
static constexpr uint8_t effect_only_roles = paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT);

/* A map-coverage operator only exists as an item of a row's mask; it is never a row or a content
 * correction of its own. */
static constexpr uint8_t mask_item_roles = paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM);

/* New kinds are added here. A kind that needs the row bake is registered with a `TODO:` naming
 * it, so what is deferred shows up in a search of this file. The generator kinds are the
 * exception: their schemas live in `paint_layers_kind_generators.cc`, beside the generator table
 * they answer for, and #paint_layer_kind_find walks them right after this list. */
static const char *const text_decal_no_stroke_reason =
    "A Text Decal is generated from its Text object and cannot be painted; edit the text instead";
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
    /* A node group the user built (a #MaterialPaintLayerSource kind reading
     * #MaterialPaintLayer::kind_tree): the row lays what the group's `Result` (or its per-channel
     * outputs) show, computed over the map by the shader-bake. Read, not painted, like a Material
     * row; the parameters are the tree's interface sockets, the schema is the dynamic one. The
     * row is baked like a heavy generator's: the bake map is what the live graph lays (the first
     * pass instantiates no user tree there), so a fresh bake is what the viewport shows.
     *
     * A correction form is the same answer laid over another row's content
     * (#BKE_paint_layers_correction_add takes the kind), with the composite's laying branch and the
     * shader-bake sweep already serving it. The mask-item form still waits for its describe and
     * graph branch, so it is not offered yet. */
    {MA_PAINT_LAYER_SOURCE_NODE_GROUP,
     0,
     "Node Group",
     PaintLayerKindCategory::Source,
     paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER) |
         paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT),
     nullptr,
     0,
     uint32_t(PAINT_LAYER_KIND_READS_SOURCE | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_DYNAMIC_SCHEMA)},
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
     invert_params,
     int(std::size(invert_params)),
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
    /* A soft threshold: the contrast opens the position into a smoothstep ramp, so a mask grows
     * and shrinks by eating its dark side away rather than cutting at one point. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_HISTOGRAM_SCAN,
     "Histogram Scan",
     PaintLayerKindCategory::Operator,
     filter_roles,
     histogram_scan_params,
     int(std::size(histogram_scan_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    /* The whole tonal range rolled by one amount: `fract(x + position)`, cyclic, so a full turn
     * is the value again. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_HISTOGRAM_SHIFT,
     "Histogram Shift",
     PaintLayerKindCategory::Operator,
     filter_roles,
     histogram_shift_params,
     int(std::size(histogram_shift_params)),
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
    /* A CPU-computed effect: either it reads a neighbourhood of the map it works on (a Blur), which
     * the live graph cannot express, or its sequence is not the graph's (a Noise). As an EFFECT it
     * reads the row's colour and the effect cache computes it, so a row that carries one is baked
     * for the live graph; as a MASK ITEM it reads the grey the mask has up to it, through the mask
     * effect's own cache the same way. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_BLUR,
     "Blur",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     blur_params,
     int(std::size(blur_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_RESULT_HAS_ALPHA |
              PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* A line average at an angle: one pass of one kernel, not separable like the box blur's pair,
     * which reaches both ways at once along the line it samples. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL,
     "Blur Directional",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     blur_directional_params,
     int(std::size(blur_directional_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_RESULT_HAS_ALPHA |
              PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* A shadow cast off the row's own shape: the silhouette is spread, blurred and offset before
     * it is tinted. Its result is a layer with an alpha of its own, so the shadow shows on the
     * empty map around a stroke. In a mask the tint is a grey of itself (the mask kind normalizes),
     * and the shape always reads the luminance, for the grey carries no alpha. The plan mirrors
     * the kernel -- the silhouette spread by the extremum pair, blurred, offset and tinted by the
     * tail -- so the combined preview computes it on the GPU like the Bevel does. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_DROP_SHADOW,
     "Drop Shadow",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     drop_shadow_params,
     int(std::size(drop_shadow_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_MASK_NEEDS_LUMA |
              PAINT_LAYER_KIND_RESULT_HAS_ALPHA | PAINT_LAYER_KIND_EFFECT_WHOLE_MAP |
              PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* A band along the border of the shape: a distance field from the border, banded by the width
     * and softened by the blur -- the Bevel's transform, taken as the edge band rather than the
     * ramp. A content row's result is a ring layer of its own (alpha = band, colour = Color) read
     * from the alpha or the luminance the source names, so it shows on the empty map around the
     * islands. A mask's grey has no alpha, so the shape is the luminance the threshold crosses and
     * the band is a grey. The plan floods the seed field and reads the band off it, so the
     * combined preview computes it on the GPU like the Bevel does. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_MASK_OUTLINE,
     "Mask Outline",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     mask_outline_params,
     int(std::size(mask_outline_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_RESULT_HAS_ALPHA |
              PAINT_LAYER_KIND_EFFECT_WHOLE_MAP | PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* What a continuous border encloses: the plan floods the label field (the border and the map's
     * edge the background, the flooding steps over it), diffuses the border's colours inward and
     * writes the fill out with the coverage the flooding named, so the combined preview computes it
     * on the GPU like the Bevel does. The parameterizations the flood cannot express stay with the
     * CPU, the plan builder refusing them with an empty plan: the debug's ramp of region numbers,
     * the largest-region classification (a reduction over the regions the flood does not walk), and
     * the source mode without an iteration to diffuse with. A content row's result is a layer of
     * its own (alpha one where a region is filled): a hole between the strokes has no coverage, so
     * a colour-only fill would stay invisible there. In a mask the fill is a grey of itself (the
     * kind normalizes) and the border always reads the luminance. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_FILL_AREA,
     "Fill Area",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     fill_area_params,
     int(std::size(fill_area_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_MASK_NEEDS_LUMA |
              PAINT_LAYER_KIND_RESULT_HAS_ALPHA | PAINT_LAYER_KIND_EFFECT_WHOLE_MAP |
              PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* One channel of an assigned image (an ORM map, say) laid as a grey in the channels the row
     * works in: the resample of the slot is a whole-map pass the effect cache computes, so it
     * needs the bake like the Bevel does. The row's own slot is the one map it reads: the
     * dependency walk, the cache key and the drop slot all read the registry's slot flag. One pass
     * fetches the channel out of the slot map, so the combined preview computes it on the GPU like
     * the Noise does, the slot bound beside the effect's own ping-pong pair. In a mask the
     * extracted channel is the coverage the grey carries; the result is a grey either way, so no
     * luminance pass is asked of the mask form. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT,
     "Channel Split",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     channel_split_params,
     int(std::size(channel_split_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_RESULT_HAS_ALPHA |
              PAINT_LAYER_KIND_EFFECT_WHOLE_MAP),
     /* #PaintLayerKindInfo::map_paintable, no_stroke_reason, reads_image_slot. */
     false,
     nullptr,
     true},
    /* A Normal to Mask rebuilds one field (Height, Edges, Slope) from a tangent normal and cuts it
     * into a grey by the threshold. The normal it reads is the one the element references: as a
     * MASK ITEM the element is an Anchor Point reader itself (READS_ANCHOR_POINT, the source's
     * Normal channel), and as an EFFECT the input is the row's own Normal content, which an
     * Anchor Point reader row already lays from its source. The field solve (a Poisson
     * integration the graph has no node for) is CPU-only: NEEDS_BAKE without GPU_COMPUTED, so the
     * combined preview falls back to the CPU the way a Bevel or a Glow does. The kernel consumes
     * the Normal's encoding and lays a grey, so it is not NORMAL_SAFE: it reads the Normal
     * (READS_CHANNEL) and never takes part in it, and the grey it lays goes to the colour channels
     * alone (WRITES_ONLY_COLOUR_CHANNELS, which makes the effect's own Normal record the input's
     * switch).
     * No #PAINT_LAYER_KIND_EFFECT_REGION_EXACT: the field is normalized by a robust 1-99
     * percentile window over the whole buffer (even the Slope mode's locally built field), and the
     * Height mode solves it with a multi-level Poisson relaxation, so a value depends on the whole
     * canvas. A crop would move the window and the far texels, which no finite halo can restore --
     * the regional path must fall back to the full compute. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_NORMAL_TO_MASK,
     "Normal to Mask",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     normal_to_mask_params,
     int(std::size(normal_to_mask_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_READS_ANCHOR_POINT | PAINT_LAYER_KIND_RESULT_HAS_ALPHA),
     /* #PaintLayerKindInfo::map_paintable, no_stroke_reason, reads_image_slot. */
     false,
     nullptr,
     false,
     /* #PaintLayerKindInfo::reads_channel, writes_only_colour_channels. */
     int8_t(PAINT_MATERIAL_CHANNEL_NORMAL),
     true},
    /* A filter the user built as a node group (a #MaterialPaintLayer::kind_tree): the group
     * answers `Result` for the `Input` the channel content (or the mask grey) feeds it, and every
     * other input but `UV` and `Mask` is a parameter of the dynamic schema. A per-texel effect
     * like the pointwise filters: the shader-bake computes it through the effect cache, so the
     * bake and the composite read a result map like any other CPU-computed kind. Not
     * #PAINT_LAYER_KIND_GPU_COMPUTED: the live graph has no branch that emits this kind's tree,
     * which is what #PAINT_LAYER_KIND_NO_GPU_BRANCH refuses (the viewport does not instantiate
     * the tree), and the CPU preview answers for that path. The combined preview is not refused
     * by it: its refusal matrix exempts a precomputed result, and
     * #combined_effect_bake_upload reads the CPU cache into the result texture, so the kind
     * previews through that upload rather than through a pass of its own. Combined preview with
     * #combined_effect_bake_upload is implemented in Patch 6 (combined_preview.patch); before
     * that, the kind takes the CPU-only preview path. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_NODE_GROUP,
     "Node Group",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     nullptr,
     0,
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_MASK_NEEDS_LUMA | PAINT_LAYER_KIND_NO_GPU_BRANCH |
              PAINT_LAYER_KIND_DYNAMIC_SCHEMA)},
    /* The Height laid as a normal: the Sobel gradients of the row's Height, encoded as the
     * tangent-space normal. The one cross-channel effect that lays the channel it reads: it reads
     * the Height (READS_CHANNEL) and lays into the Normal channel, the only one it filters, which
     * is why it is also #PAINT_LAYER_KIND_NORMAL_SAFE. Its output is not colour-only, so its input
     * stays the row's Height content rather than a switch of its own. One 3x3 pass over the
     * height, so the combined preview computes it on the GPU like the Edge Detect does. A content
     * correction only: a mask item has no channel of its own to lay a normal into. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL,
     "Height To Normal",
     PaintLayerKindCategory::Operator,
     effect_only_roles,
     height_to_normal_params,
     int(std::size(height_to_normal_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_NORMAL_SAFE |
              PAINT_LAYER_KIND_EFFECT_WHOLE_MAP |
              PAINT_LAYER_KIND_EFFECT_REGION_EXACT),
     /* #PaintLayerKindInfo::map_paintable, no_stroke_reason, reads_image_slot. */
     false,
     nullptr,
     false,
     /* #PaintLayerKindInfo::reads_channel, writes_only_colour_channels. */
     int8_t(PAINT_MATERIAL_CHANNEL_HEIGHT),
     false},
    /* The Hermite interpolation between the edges: the Map Range node's own Smoothstep, the
     * smoother extraction the Histogram Scan's threshold opens into. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_SMOOTHSTEP,
     "Smoothstep",
     PaintLayerKindCategory::Operator,
     filter_roles,
     smoothstep_params,
     int(std::size(smoothstep_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    /* The distance from the middle scales and the result shifts: the two sliders a brightness and
     * contrast control carries. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_CONTRAST_LUMINOSITY,
     "Contrast Luminosity",
     PaintLayerKindCategory::Operator,
     filter_roles,
     contrast_luminosity_params,
     int(std::size(contrast_luminosity_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    /* The invert, then the offset, then the multiply: the order the filter's own note pins, where
     * a zero offset result annihilates the multiply at that point. The result stays open above
     * one, the same policy the Exposure carries. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_HEIGHT_ADJUST,
     "Height Adjust",
     PaintLayerKindCategory::Operator,
     filter_roles,
     height_adjust_params,
     int(std::size(height_adjust_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    /* The map cut into blocks of one colour: the kernel reads each block once, so the live graph
     * cannot express it and the row is baked for it. One pass averages the block each texel
     * belongs to, so the combined preview computes it on the GPU like the Noise does. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_PIXELATE,
     "Pixelate",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     pixelate_params,
     int(std::size(pixelate_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_EFFECT_WHOLE_MAP)},
    /* The hue and saturation shift the way the Hue/Saturation node does; the mode says what the
     * lightness moves, the Regular's value channel or the Perceptive lift toward white. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_HSL_PERSPECTIVE,
     "HSL Perspective",
     PaintLayerKindCategory::Operator,
     filter_roles,
     hsl_perspective_params,
     int(std::size(hsl_perspective_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    /* The luminance-driven edge blend: one pass of one 3x3 kernel, so the combined preview
     * computes it on the GPU like the Edge Detect does. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_FXAA,
     "FXAA",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     nullptr,
     0,
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* The walk along the slope of a source: the samples the quality names take the direction the
     * gradient names, the blending mode saying what they accumulate. The plan lays the driver's
     * blur, the driver itself and the walk over the original, so the combined preview computes it
     * on the GPU like the Bevel does. The row is baked for the graph either way. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_BLUR_SLOPE,
     "Blur Slope",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     blur_slope_params,
     int(std::size(blur_slope_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_EFFECT_WHOLE_MAP)},
    /* Unsharp masking keeps an HDR value above one: only the negative side is clamped. The same
     * open-above-one policy as Exposure and High Pass, unlike Noise and Channel Mixer. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_SHARPEN,
     "Sharpen",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     sharpen_params,
     int(std::size(sharpen_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* The detail is a difference against a blurred copy, so an HDR value above one stays above it;
     * only the negative side is clamped. The same open-above-one policy as Exposure and Sharpen. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_HIGH_PASS,
     "High Pass",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     high_pass_params,
     int(std::size(high_pass_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* The Sobel gradient magnitude: a full-range step reads as one, a flat area as zero. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_EDGE_DETECT,
     "Edge Detect",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     nullptr,
     0,
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* A diagonal 3x3 lighting kernel: a flat colour is its own result, an edge takes the light. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_EMBOSS,
     "Emboss",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     nullptr,
     0,
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* The extreme of a square window per channel: min and max are separable, so the kernel is one
     * pass per axis like the box blur's. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_ERODE_DILATE,
     "Erode / Dilate",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     erode_dilate_params,
     int(std::size(erode_dilate_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* A glow screens its colour under the content, so it stays bounded where the colour is. Its
     * source is choked to [0, 1] by the same bound: an HDR texel above one does not brighten the
     * glow, unlike the open-topped Sharpen and High Pass. The same clamp zeros the mask at a
     * spread of one, so the slider stops at 0.99: the maximum keeps the narrow, sharp remnant over
     * the brightest texels instead of choking the glow away. The plan mirrors the kernel -- the
     * luminance choked, blurred and screened under the colour -- so the combined preview computes
     * it on the GPU like the High Pass does. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_GLOW,
     "Glow",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     glow_params,
     int(std::size(glow_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_RESULT_HAS_ALPHA |
              PAINT_LAYER_KIND_MASK_NEEDS_LUMA | PAINT_LAYER_KIND_EFFECT_WHOLE_MAP |
              PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* A bevel replaces the colour with a distance ramp from the content's borders: its alpha, or
     * its luminance when the source says so -- an opaque row bevels by the dark side of its paint.
     * The plan floods the distance field (the JFA passes) and reads the ramp off it, so the
     * combined preview computes it on the GPU like the High Pass does. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_BEVEL,
     "Bevel",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     bevel_params,
     int(std::size(bevel_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_EFFECT_WHOLE_MAP |
              PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* The grade moves every texel by the distance between the target colour and the source one --
     * the input's own average, which the live graph cannot express and the plan reduces down a
     * pyramid to the 1x1 apex the grade reads, so the combined preview computes it on the GPU like
     * the Bevel does. The parameterization the pyramid cannot serve stays with the CPU, the plan
     * builder refusing it with an empty plan: a picked source (the colour the contract lays in the
     * grade's params2, the one slot whose reading the plan cannot verify against the CPU kernel)
     * keeps the pick exact on the CPU. The variation rides the grade's params beside the target. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_COLOR_MATCH,
     "Color Match",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     color_match_params,
     int(std::size(color_match_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_MASK_NEEDS_LUMA |
              PAINT_LAYER_KIND_EFFECT_WHOLE_MAP |
              PAINT_LAYER_KIND_EFFECT_REGION_EXACT)},
    /* The gamma leaves a non-positive value alone and has no upper bound, so an HDR value above
     * one passes through: only the negative side is clamped. The same policy as Sharpen and High
     * Pass. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_EXPOSURE,
     "Exposure",
     PaintLayerKindCategory::Operator,
     filter_roles,
     exposure_params,
     int(std::size(exposure_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    /* A pointwise per-texel dither: it reads a texel's coordinates for its noise. The combined
     * preview's apply_filter branch receives those coordinates now, so its Deband branch computes
     * the same noise the CPU (which walks the texels) does, and the filter no longer falls back to
     * it. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_DEBAND,
     "Deband / Dither",
     PaintLayerKindCategory::Operator,
     filter_roles,
     deband_params,
     int(std::size(deband_params)),
     PAINT_LAYER_KIND_ACTS_ON_BELOW},
    /* A grain is part of the texture: the shader graph's White Noise and the CPU hash are two
     * different sequences, so the noise is an effect the cache computes once and the bake carries,
     * and no live graph, adjustment or mask form exists for it. The kernel clips its result to
     * [0, 1] like the addon's own clip, so a Noise trims an HDR map; Exposure, Sharpen and High
     * Pass, whose maths has no upper bound, only clip below zero. Kept for parity with the
     * addon. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_NOISE,
     "Noise",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     noise_params,
     int(std::size(noise_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NEEDS_BAKE |
              PAINT_LAYER_KIND_GPU_COMPUTED | PAINT_LAYER_KIND_MASK_NEEDS_LUMA)},
    /* A Normal Convert reads the encoding as numbers, not as a direction: the one filter a Normal
     * channel carries, by its own flag. The combined preview's apply_filter branch flips the green
     * and rebuilds the Z the same numbers the CPU does, so the filter no longer falls back to it. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_NORMAL_CONVERT,
     "Normal Convert",
     PaintLayerKindCategory::Operator,
     filter_roles,
     normal_convert_params,
     int(std::size(normal_convert_params)),
     uint32_t(PAINT_LAYER_KIND_ACTS_ON_BELOW | PAINT_LAYER_KIND_NORMAL_SAFE)},
    /* A Channel Mixer reads the colour as numbers in a matrix; its payload is the matrix, its
     * monochrome a plain Bool. The combined preview shader has a branch for it, like Color
     * Balance. Its apply clips the result to [0, 1] exactly as the addon's `np.clip` does, so an
     * HDR value is trimmed; the filters whose maths stays open above one (Exposure, Sharpen,
     * High Pass) do not clip there. Kept for parity with the addon. */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_CHANNEL_MIXER,
     "Channel Mixer",
     PaintLayerKindCategory::Operator,
     filter_roles,
     channel_mixer_params,
     int(std::size(channel_mixer_params)),
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
    /* A transform effect does not correct the colour it reads: it moves the UV the row it hangs on
     * reads its content at. The owner's reads go through the effect's fold and transform first,
     * wherever a map of the owner is sampled, so the effect carries no channel records and takes
     * no part in a pointwise walk. Its parameters are its own #MaterialPaintLayerMapping -- the
     * same struct a Fill keeps, read by the same panel, gizmo and value sync -- so the kind has
     * none of its own. A Transform Mapping reads the offset, rotation, scale and wrap; a Mirror
     * reads the fold's axes and center alone. As an EFFECT the effect transforms the row's
     * content, as a MASK ITEM the coverage the mask lays; it is never a row of the stack (an
     * adjustment would have to resample the whole composite below it, which no backend builds). */
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_TRANSFORM,
     "Transform Mapping",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     nullptr,
     0,
     PAINT_LAYER_KIND_TRANSFORMS_INPUT},
    {MA_PAINT_LAYER_SOURCE_FILTER,
     MA_PAINT_LAYER_FILTER_MIRROR,
     "Mirror",
     PaintLayerKindCategory::Operator,
     effect_or_mask_roles,
     nullptr,
     0,
     PAINT_LAYER_KIND_TRANSFORMS_INPUT},
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
    /* A Text Decal effect is a Fill-like row: each channel holds a constant value or the texture
     * the user dropped on it (#PAINT_LAYER_KIND_FLAT_VALUE, so a value edit is the cheap
     * values-only sync and a Fill's rules for textures, mapping and strokes apply). The rasterized
     * text of a Text object is only the mask of the whole effect, kept in the payload's coverage
     * map, which is why the kind projects "its own map" through its frame even when every channel
     * is a flat value. The coverage belongs to the object, so no stroke may write into it
     * (#PAINT_LAYER_KIND_NO_STROKES). Never a row of the stack: a content correction of a row. */
    {MA_PAINT_LAYER_SOURCE_TEXT,
     MA_PAINT_LAYER_TEXT_EFFECT,
     "Text Decal",
     PaintLayerKindCategory::Source,
     uint8_t(paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT)),
     nullptr,
     0,
     PAINT_LAYER_KIND_FLAT_VALUE | PAINT_LAYER_KIND_PROJECTS_OWN_MAP | PAINT_LAYER_KIND_NO_STROKES,
     false,
     text_decal_no_stroke_reason},
    /* The mask form: the text's coverage is the grey of the item's single Base Color map. Not a
     * flat value: the item has no constant of its own, and it is never a stroke target. */
    {MA_PAINT_LAYER_SOURCE_TEXT,
     MA_PAINT_LAYER_TEXT_MASK,
     "Mask Text Decal",
     PaintLayerKindCategory::Source,
     uint8_t(paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM)),
     nullptr,
     0,
     PAINT_LAYER_KIND_PROJECTS_OWN_MAP | PAINT_LAYER_KIND_NO_STROKES,
     false,
     text_decal_no_stroke_reason},
};

const PaintLayerKindInfo *paint_layer_kind_find(const int8_t source, const int16_t subtype)
{
  for (const PaintLayerKindInfo &info : kinds) {
    if (info.source == source && info.subtype == subtype) {
      return &info;
    }
  }
  /* The generators answer to the same registry: their schemas live beside the generator table
   * (paint_layers_kind_generators.cc), and a generator kind is found here like any other kind. */
  for (const PaintLayerKindInfo &info : paint_layer_kind_generator_kinds()) {
    if (info.source == source && info.subtype == subtype) {
      return &info;
    }
  }
  return nullptr;
}

const PaintLayerKindInfo *paint_layer_kind_of(const MaterialPaintLayer &row)
{
  const PaintLayerKindInfo *info = paint_layer_kind_find(row.source, row.kind_subtype);
  if (info == nullptr || (info->flags & PAINT_LAYER_KIND_DYNAMIC_SCHEMA) == 0) {
    return info;
  }
  /* A dynamic kind reads its schema from the definition the row names: the tree's interface for
   * the node-group kinds, the chain definition's members for a chain. The definition missing
   * (a linked tree out of reach, a deleted asset) answers the static entry, whose empty schema is
   * the truth a reader without a definition can state. */
  return paint_layer_kind_dynamic_of(row);
}

bool paint_layer_kind_role_allowed(const MaterialPaintLayer &row)
{
  const PaintLayerKindInfo *info = paint_layer_kind_of(row);
  if (info == nullptr || (info->allowed_roles & paint_layer_role_bit(row.role)) == 0) {
    return false;
  }
  /* A NEEDS_BAKE kind computes itself through an effect cache: an effect against the row's colour,
   * a mask item against the grey the mask has built up to it (the mask effect cache). A row of one
   * would be described from the parameters alone, and those carry no colour. A source kind is the
   * exception: its content is the map the bake lays, so it is a layer like a Material row. */
  if ((info->flags & PAINT_LAYER_KIND_NEEDS_BAKE) != 0 &&
      row.role == MA_PAINT_LAYER_ROLE_LAYER &&
      (info->flags & PAINT_LAYER_KIND_READS_SOURCE) == 0)
  {
    return false;
  }
  return true;
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
           PaintLayerKindParamType::Vector,
           PaintLayerKindParamType::Ramp,
           PaintLayerKindParamType::Curves,
           PaintLayerKindParamType::ColorBalance,
           PaintLayerKindParamType::ChannelMixer))
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
  IDProperty *prop = existing;
  if (existing == nullptr || existing->type != IDP_ARRAY || existing->subtype != IDP_FLOAT ||
      existing->len != 4)
  {
    float default_color[4];
    copy_v4_v4(default_color, param.default_color);
    prop = bke::idprop::create(param.name, Span<float>(default_color, 4)).release();
    IDP_ReplaceInGroup(group, prop);
  }
  /* The label travels with the property, as a scalar's does: without it the parameter is drawn
   * under its identifier ("color") instead of its name ("Fill Color"). */
  if (prop->ui_data == nullptr) {
    IDPropertyUIDataFloat *ui_data = reinterpret_cast<IDPropertyUIDataFloat *>(
        IDP_ui_data_ensure(prop));
    ui_data->base.description = BLI_strdup(param.label);
  }
  return prop;
}

/** The stored float[3] array of a Vector parameter, made at the schema default when it has none.
 * The default rides in the first three slots of #PaintLayerKindParam::default_color, the way a
 * Color's four ride there whole. */
static IDProperty *kind_param_ensure_vector(MaterialPaintLayer &row,
                                            const PaintLayerKindParam &param)
{
  IDProperty *group = kind_param_group_ensure(row);
  IDProperty *existing = IDP_GetPropertyFromGroup(group, param.name);
  IDProperty *prop = existing;
  if (existing == nullptr || existing->type != IDP_ARRAY || existing->subtype != IDP_FLOAT ||
      existing->len != 3)
  {
    const float default_vector[3] = {param.default_color[0],
                                     param.default_color[1],
                                     param.default_color[2]};
    prop = bke::idprop::create(param.name, Span<float>(default_vector, 3)).release();
    IDP_ReplaceInGroup(group, prop);
  }
  if (prop->ui_data == nullptr) {
    IDPropertyUIDataFloat *ui_data = reinterpret_cast<IDPropertyUIDataFloat *>(
        IDP_ui_data_ensure(prop));
    ui_data->base.description = BLI_strdup(param.label);
    ui_data->min = ui_data->soft_min = param.min;
    ui_data->max = ui_data->soft_max = param.max;
  }
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
        case PaintLayerKindParamType::Vector:
          kind_param_ensure_vector(row, param);
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
        case PaintLayerKindParamType::ChannelMixer:
          BKE_paint_layers_channel_mixer_ensure(row);
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

/** The float[3] value of a Vector parameter \a index, or the schema default for an unknown one.
 * The default rides in the schema's default_color, the way a Color's does. */
void paint_layer_kind_param_vector_get(const MaterialPaintLayer &row,
                                       const int index,
                                       float r_vector[3])
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  if (param == nullptr || param->type != PaintLayerKindParamType::Vector) {
    zero_v3(r_vector);
    return;
  }
  const IDProperty *prop = (row.system_properties != nullptr) ?
                               IDP_GetPropertyFromGroup(row.system_properties, param->name) :
                               nullptr;
  if (prop == nullptr || prop->type != IDP_ARRAY || prop->subtype != IDP_FLOAT ||
      prop->len != 3)
  {
    copy_v3_v3(r_vector, param->default_color);
    return;
  }
  copy_v3_v3(r_vector, IDP_array_float_get(prop));
}

bool paint_layer_kind_param_vector_set(MaterialPaintLayer &row,
                                       const int index,
                                       const float vector[3])
{
  const PaintLayerKindParam *param = kind_param_find(row, index);
  if (param == nullptr || param->type != PaintLayerKindParamType::Vector) {
    return false;
  }
  IDProperty *prop = kind_param_ensure_vector(row, *param);
  copy_v3_v3(IDP_array_float_get(prop), vector);
  return true;
}

bool BKE_paint_layers_ramp_supported(const MaterialPaintLayer &row)
{
  return paint_layer_kind_has_ramp_param(row);
}

bool BKE_paint_layers_normal_channel_supported(const MaterialPaintLayer &row)
{
  return paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_NORMAL_SAFE);
}

bool BKE_paint_layers_reads_height(const MaterialPaintLayer &row)
{
  return paint_layer_effect_reads_height(row);
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
    /* After the tag, which clears it: see #BKE_paint_layers_generator_ramp_edited. */
    ma.paint_layers_flag |= MA_PAINT_LAYERS_RAMP_ONLY;
  }
  return found;
}

bool paint_layer_kind_has_payload_param(const MaterialPaintLayer &row,
                                        const PaintLayerKindParamType type)
{
  const PaintLayerKindInfo *info = paint_layer_kind_of(row);
  if (info == nullptr) {
    return false;
  }
  for (const int i : IndexRange(info->params_num)) {
    if (info->params[i].type == type) {
      return true;
    }
  }
  return false;
}

bool paint_layer_kind_has_ramp_param(const MaterialPaintLayer &row)
{
  return paint_layer_kind_has_payload_param(row, PaintLayerKindParamType::Ramp);
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
  return paint_layer_kind_has_payload_param(row, PaintLayerKindParamType::Curves);
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
  return paint_layer_kind_has_payload_param(row, PaintLayerKindParamType::ColorBalance);
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
  /* The member initializers of #MaterialPaintLayerColorBalance are the neutral balance: every
   * method's identity (lift 0, gamma 1, gain 1; slope 1, offset 0, power 1). */
  return static_cast<MaterialPaintLayerColorBalance *>(
      paint_layer_payload_ensure(row, PaintLayerKindParamType::ColorBalance));
}

const MaterialPaintLayerColorBalance &paint_layer_color_balance_or_neutral(
    const MaterialPaintLayer &row)
{
  static const MaterialPaintLayerColorBalance neutral = {};
  return (row.color_balance != nullptr) ? *row.color_balance : neutral;
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
  /* The balance reaches the generated graph as the values of three group inputs and the composite
   * stack's hash carries the folded numbers, so the registry's value-only tag is the whole story:
   * no group is rebuilt, and the bake flags follow the hash. */
  return paint_layer_payload_edited(ma, data, PaintLayerKindParamType::ColorBalance);
}

bool paint_layer_kind_has_channel_mixer_param(const MaterialPaintLayer &row)
{
  return paint_layer_kind_has_payload_param(row, PaintLayerKindParamType::ChannelMixer);
}

bool BKE_paint_layers_channel_mixer_supported(const MaterialPaintLayer &row)
{
  return paint_layer_kind_has_channel_mixer_param(row);
}

bool BKE_paint_layers_color_match_supported(const MaterialPaintLayer &row)
{
  return row.source == MA_PAINT_LAYER_SOURCE_FILTER &&
         row.kind_subtype == MA_PAINT_LAYER_FILTER_COLOR_MATCH;
}

bool BKE_paint_layers_reads_image_slot(const MaterialPaintLayer &row)
{
  const PaintLayerKindInfo *kind = paint_layer_kind_of(row);
  return kind != nullptr && kind->reads_image_slot;
}

MaterialPaintLayerChannelMixer *BKE_paint_layers_channel_mixer_ensure(MaterialPaintLayer &row)
{
  /* The member initializers of #MaterialPaintLayerChannelMixer are the neutral matrix: the identity
   * rows and zero constants. */
  return static_cast<MaterialPaintLayerChannelMixer *>(
      paint_layer_payload_ensure(row, PaintLayerKindParamType::ChannelMixer));
}

const MaterialPaintLayerChannelMixer *BKE_paint_layers_channel_mixer_get(
    const MaterialPaintLayer &row)
{
  return row.channel_mixer;
}

uint64_t paint_layer_channel_mixer_hash(const MaterialPaintLayerChannelMixer &mixer)
{
  uint64_t hash = 1469598103934665603ull;
  const auto mix_float = [&](const float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    hash ^= bits;
    hash *= 1099511628211ull;
  };
  for (const float value : mixer.red_row) {
    mix_float(value);
  }
  for (const float value : mixer.green_row) {
    mix_float(value);
  }
  for (const float value : mixer.blue_row) {
    mix_float(value);
  }
  for (const float value : mixer.constants) {
    mix_float(value);
  }
  return hash;
}

bool BKE_paint_layers_channel_mixer_edited(Material &ma, const void *data)
{
  /* The matrix reaches the generated graph as the values of four group inputs and the composite
   * stack's hash carries the folded numbers, so the registry's value-only tag is the whole story:
   * no group is rebuilt, and the bake flags follow the hash. */
  return paint_layer_payload_edited(ma, data, PaintLayerKindParamType::ChannelMixer);
}

/* -------------------------------------------------------------------- */
/** \name Kind payload registry
 * \{ */

namespace {

template<typename T> void *payload_create()
{
  return MEM_new<T>(__func__);
}

template<typename T> void payload_destroy(void *payload)
{
  MEM_delete(static_cast<T *>(payload));
}

template<typename T> void *payload_copy(const void *payload)
{
  return MEM_dupalloc(static_cast<const T *>(payload));
}

template<typename T> void payload_write(BlendWriter *writer, const void *payload)
{
  writer->write_struct(static_cast<const T *>(payload));
}

template<typename T> void payload_read(BlendDataReader *reader, void **slot)
{
  BLO_read_struct(reader, T, slot);
}

uint64_t payload_color_balance_hash(const void *payload)
{
  return paint_layer_color_balance_hash(
      *static_cast<const MaterialPaintLayerColorBalance *>(payload));
}

uint64_t payload_channel_mixer_hash(const void *payload)
{
  return paint_layer_channel_mixer_hash(
      *static_cast<const MaterialPaintLayerChannelMixer *>(payload));
}

void payload_value_edited(Material &ma)
{
  BKE_paint_layers_tag_value_edited(ma);
}

}  // namespace

static const PaintLayerPayloadInfo payload_infos[] = {
    {PaintLayerKindParamType::ColorBalance,
     offsetof(MaterialPaintLayer, color_balance),
     payload_create<MaterialPaintLayerColorBalance>,
     payload_destroy<MaterialPaintLayerColorBalance>,
     payload_copy<MaterialPaintLayerColorBalance>,
     payload_color_balance_hash,
     payload_write<MaterialPaintLayerColorBalance>,
     payload_read<MaterialPaintLayerColorBalance>,
     payload_value_edited},
    {PaintLayerKindParamType::ChannelMixer,
     offsetof(MaterialPaintLayer, channel_mixer),
     payload_create<MaterialPaintLayerChannelMixer>,
     payload_destroy<MaterialPaintLayerChannelMixer>,
     payload_copy<MaterialPaintLayerChannelMixer>,
     payload_channel_mixer_hash,
     payload_write<MaterialPaintLayerChannelMixer>,
     payload_read<MaterialPaintLayerChannelMixer>,
     payload_value_edited},
};

const PaintLayerPayloadInfo *paint_layer_payload_info(const PaintLayerKindParamType type)
{
  for (const PaintLayerPayloadInfo &info : payload_infos) {
    if (info.type == type) {
      return &info;
    }
  }
  return nullptr;
}

/** The address of the payload pointer \a info describes in \a row, for the read and copy passes. */
static void **payload_slot(MaterialPaintLayer &row, const PaintLayerPayloadInfo &info)
{
  return reinterpret_cast<void **>(reinterpret_cast<char *>(&row) + info.offset);
}

static const void *payload_slot_get(const MaterialPaintLayer &row,
                                    const PaintLayerPayloadInfo &info)
{
  return *reinterpret_cast<const void *const *>(reinterpret_cast<const char *>(&row) +
                                                info.offset);
}

void *paint_layer_payload_ensure(MaterialPaintLayer &row, const PaintLayerKindParamType type)
{
  const PaintLayerPayloadInfo *info = paint_layer_payload_info(type);
  if (info == nullptr || !paint_layer_kind_has_payload_param(row, type)) {
    return nullptr;
  }
  void **slot = payload_slot(row, *info);
  if (*slot == nullptr) {
    *slot = info->create();
  }
  return *slot;
}

void paint_layer_payload_free(MaterialPaintLayer &row)
{
  for (const PaintLayerPayloadInfo &info : payload_infos) {
    void **slot = payload_slot(row, info);
    if (*slot != nullptr) {
      info.destroy(*slot);
      *slot = nullptr;
    }
  }
}

void paint_layer_payload_copy(const MaterialPaintLayer &src, MaterialPaintLayer &dst)
{
  for (const PaintLayerPayloadInfo &info : payload_infos) {
    const void *payload = payload_slot_get(src, info);
    *payload_slot(dst, info) = (payload != nullptr) ? info.copy(payload) : nullptr;
  }
}

void paint_layer_text_decal_copy(MaterialPaintLayer &dst,
                                 const MaterialPaintLayer &src,
                                 const bool take_object_user)
{
  if (src.text_decal == nullptr) {
    return;
  }
  MaterialPaintLayerText *payload = MEM_new<MaterialPaintLayerText>(__func__);
  dst.text_decal = payload;
  *payload = dna::shallow_copy(*src.text_decal);
  /* The raster is rebuilt for the copy: a shared coverage map would be resized and rewritten by two
   * rows. */
  payload->coverage = nullptr;
  if (take_object_user && payload->text_object != nullptr) {
    id_us_plus(&payload->text_object->id);
  }
}

uint64_t paint_layer_payload_hash(const MaterialPaintLayer &row)
{
  uint64_t hash = 1469598103934665603ull;
  for (const PaintLayerPayloadInfo &info : payload_infos) {
    if (const void *payload = payload_slot_get(row, info)) {
      hash ^= info.hash(payload);
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

void paint_layer_payload_write(BlendWriter &writer, const MaterialPaintLayer &layer)
{
  for (const PaintLayerPayloadInfo &info : payload_infos) {
    if (const void *payload = payload_slot_get(layer, info)) {
      info.write(&writer, payload);
    }
  }
}

void paint_layer_payload_read(BlendDataReader &reader, MaterialPaintLayer &layer)
{
  for (const PaintLayerPayloadInfo &info : payload_infos) {
    void **slot = payload_slot(layer, info);
    info.read(&reader, slot);
    if (*slot == nullptr && paint_layer_kind_has_payload_param(layer, info.type)) {
      *slot = info.create();
    }
  }
}

bool paint_layer_payload_edited(Material &ma,
                                const void *data,
                                const PaintLayerKindParamType type)
{
  if (data == nullptr) {
    return false;
  }
  const PaintLayerPayloadInfo *info = paint_layer_payload_info(type);
  if (info == nullptr) {
    return false;
  }
  const auto owns = [&](const MaterialPaintLayer &row) {
    return payload_slot_get(row, *info) == data;
  };
  bool found = false;
  for (MaterialPaintLayer &row : ma.paint_layers) {
    if (paint_layer_payload_edited_walk(row, owns)) {
      found = true;
      break;
    }
  }
  if (found && info->edited != nullptr) {
    info->edited(ma);
  }
  return found;
}

/** \} */

}  // namespace blender
