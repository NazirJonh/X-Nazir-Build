/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The kind tables the paint layers source reads: the look a row wears and the filter lists the Add
 * Effect menu offers. Kept apart from the row builder so the source stays within the module's size
 * budget; see `outliner_stack_paint_filter_table.hh`.
 */

#include "outliner_stack_paint_filter_table.hh"

#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "UI_resources.hh"

namespace blender::ed::outliner {

namespace paint_glyph {
const uint32_t BRUSH = 0xE3AE;          /* brush */
const uint32_t FILL = 0xE997;           /* colors */
const uint32_t GRADIENT = 0xE3E9;       /* gradient */
const uint32_t TEXTURE = 0xE421;        /* texture */
const uint32_t INVERT = 0xE891;         /* invert_colors */
const uint32_t LEVELS = 0xE429;         /* tune */
const uint32_t BRIGHTNESS = 0xE3F6;     /* exposure */
/* Hue/Saturation/Value's own lens, not `colors`: that one is the Fill layer's. */
const uint32_t HSL = 0xE40A;            /* color_lens */
const uint32_t ANCHOR = 0xF1CD;         /* anchor */
const uint32_t CELL_TOWER = 0xEBBA;     /* cell_tower: the Anchor Point reader */
const uint32_t COLORIZE = 0xE3B8;       /* colorize */
const uint32_t SELECT_ALL = 0xE162;     /* select_all */
/* The procedural patterns (Noise, Voronoi, ...), kept apart from the Gradient's own glyph. */
const uint32_t PATTERN = 0xE3A5;        /* blur_on */
/* The Fill Area and Mask Outline effects, apart from the Fill layer's own glyph. */
const uint32_t FILL_AREA = 0xF873;
const uint32_t MASK_OUTLINE = 0xE1AD;
}  // namespace paint_glyph

/**
 * The built-in gradient icon: a Color Theme icon used as the nearest stand-in for a ramp, the same
 * one the Gradient tool wears. Rows draw the `gradient` glyph instead where it exists; this is the
 * fallback (headless builds, and Python's `icon=`), kept here once for the rows and the Add table.
 */
constexpr int PAINT_GRADIENT_ICON = ICON_COLORSET_13_VEC;

/**
 * The filter's own look: an icon with a glyph for every filter the stack has one for. The default
 * is the generic adjustment arrow, kept for a filter whose subtype the UI does not know yet.
 */
static PaintKindLook paint_filter_look(const int16_t filter)
{
  switch (filter) {
    case MA_PAINT_LAYER_FILTER_INVERT:
      return {ICON_ARROW_LEFTRIGHT, paint_glyph::INVERT};
    case MA_PAINT_LAYER_FILTER_LEVELS:
      return {ICON_SEQ_HISTOGRAM, 0};
    case MA_PAINT_LAYER_FILTER_HSL:
      return {ICON_COLOR, paint_glyph::HSL};
    case MA_PAINT_LAYER_FILTER_BRIGHTNESS_CONTRAST:
      return {ICON_COLOR, paint_glyph::BRIGHTNESS};
    case MA_PAINT_LAYER_FILTER_COLOR_RAMP:
      /* No glyph of its own yet: the icon alone keeps it apart from Invert's arrow. */
      return {ICON_COLOR, 0};
    case MA_PAINT_LAYER_FILTER_POSTERIZE:
      /* No glyph of its own yet: the icon alone keeps it apart from Invert's arrow. */
      return {ICON_MOD_SMOOTH, 0};
    case MA_PAINT_LAYER_FILTER_MATH:
      /* No glyph of its own yet: the icon alone keeps it apart from Invert's arrow. */
      return {ICON_DRIVER, 0};
    case MA_PAINT_LAYER_FILTER_THRESHOLD:
      /* No glyph of its own yet: the icon alone keeps it apart from Invert's arrow. */
      return {ICON_MOD_THICKNESS, 0};
    case MA_PAINT_LAYER_FILTER_CLAMP:
      /* No glyph of its own yet: the icon alone keeps it apart from Invert's arrow. */
      return {ICON_RESTRICT_COLOR_ON, 0};
    case MA_PAINT_LAYER_FILTER_HISTOGRAM_SCAN:
      /* No glyph of its own yet: the histogram icon names the range the filter remaps. */
      return {ICON_SEQ_HISTOGRAM, 0};
    case MA_PAINT_LAYER_FILTER_HISTOGRAM_SHIFT:
      /* No glyph of its own yet: the histogram icon names the range the filter rolls. */
      return {ICON_SEQ_HISTOGRAM, 0};
    case MA_PAINT_LAYER_FILTER_SMOOTHSTEP:
      /* No glyph of its own yet: the curve icon names the Hermite the extraction walks. */
      return {ICON_RNDCURVE, 0};
    case MA_PAINT_LAYER_FILTER_CURVES:
      /* No glyph of its own yet: the icon alone keeps it apart from Invert's arrow. */
      return {ICON_RNDCURVE, 0};
    case MA_PAINT_LAYER_FILTER_COLOR_BALANCE:
      /* The sliders Levels wore before it took the histogram. */
      return {ICON_IMAGE_ALPHA, paint_glyph::LEVELS};
    case MA_PAINT_LAYER_FILTER_BLUR:
      return {ICON_MATFLUID, 0};
    case MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL:
      /* No glyph of its own yet: the fluid icon names the streak the line average lays. */
      return {ICON_MATFLUID, 0};
    case MA_PAINT_LAYER_FILTER_SHARPEN:
      return {ICON_SHARPCURVE, 0};
    case MA_PAINT_LAYER_FILTER_HIGH_PASS:
      return {ICON_MOD_WIREFRAME, 0};
    case MA_PAINT_LAYER_FILTER_EDGE_DETECT:
      return {ICON_MOD_EDGESPLIT, 0};
    case MA_PAINT_LAYER_FILTER_EMBOSS:
      return {ICON_MOD_BEVEL, 0};
    case MA_PAINT_LAYER_FILTER_ERODE_DILATE:
      return {ICON_MOD_MULTIRES, 0};
    case MA_PAINT_LAYER_FILTER_COLOR_ID:
      return {ICON_COLOR, paint_glyph::COLORIZE};
    case MA_PAINT_LAYER_FILTER_UV_ISLAND:
      return {ICON_UV_ISLANDSEL, paint_glyph::SELECT_ALL};
    case MA_PAINT_LAYER_FILTER_EXPOSURE:
      /* No glyph of its own yet: the filter icon names a tone scale without a color of its own. */
      return {ICON_FILTER, 0};
    case MA_PAINT_LAYER_FILTER_DEBAND:
      /* No glyph of its own yet: the image-data icon names a quantization of the pixels. */
      return {ICON_IMAGE_DATA, 0};
    case MA_PAINT_LAYER_FILTER_NOISE:
      /* No glyph of its own yet: the particle-point icon names a scatter of grains. */
      return {ICON_PARTICLE_POINT, 0};
    case MA_PAINT_LAYER_FILTER_GLOW:
      return {ICON_LIGHT, 0};
    case MA_PAINT_LAYER_FILTER_BEVEL:
      return {ICON_MOD_BEVEL, 0};
    case MA_PAINT_LAYER_FILTER_COLOR_MATCH:
      return {ICON_NODE_COMPOSITING, 0};
    case MA_PAINT_LAYER_FILTER_DROP_SHADOW:
      /* No glyph of its own yet: the sun icon names the cast the silhouette takes. */
      return {ICON_LIGHT_SUN, 0};
    case MA_PAINT_LAYER_FILTER_MASK_OUTLINE:
      return {ICON_MOD_THICKNESS, paint_glyph::MASK_OUTLINE};
    case MA_PAINT_LAYER_FILTER_FILL_AREA:
      return {ICON_CURVE_PATH, paint_glyph::FILL_AREA};
    case MA_PAINT_LAYER_FILTER_NORMAL_CONVERT:
      /* No glyph of its own yet: the vertex-normals icon names what the filter converts. */
      return {ICON_NORMALS_VERTEX, 0};
    case MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL:
      /* No glyph of its own yet: the vertex-normals icon names what the height becomes. */
      return {ICON_NORMALS_VERTEX, 0};
    case MA_PAINT_LAYER_FILTER_CHANNEL_MIXER:
      /* No glyph of its own yet: the RGB icon names the channels the filter mixes. */
      return {ICON_IMAGE_RGB, 0};
    case MA_PAINT_LAYER_FILTER_TRANSFORM:
      /* No glyph of its own yet: the UV-projection icon names the placement the effect moves. */
      return {ICON_MOD_UVPROJECT, 0};
    case MA_PAINT_LAYER_FILTER_MIRROR:
      /* No glyph of its own yet: the modifier's own mirror icon names the fold. */
      return {ICON_MOD_MIRROR, 0};
  }
  return {ICON_ARROW_LEFTRIGHT, 0};
}

/**
 * The icon and glyph every row and Add entry of \a source shows, in \a role. This is the one place
 * the pair is written: the rows, the effects and the Add menu all ask here, so a kind cannot read
 * one way in the menu and another in the stack.
 */
PaintKindLook paint_look_for_source(const eMaterialPaintLayerSource source,
                                    const PaintLookRole role,
                                    const int16_t subtype)
{
  switch (source) {
    case MA_PAINT_LAYER_SOURCE_IMAGE:
      /* The mask's Add entry keeps the Grease Pencil icon the user knows; the glyph keeps it the
       * same Brush wherever it is drawn. */
      return {role == PaintLookRole::MaskItem ? ICON_GREASEPENCIL : ICON_BRUSH_DATA,
              paint_glyph::BRUSH};
    case MA_PAINT_LAYER_SOURCE_CONSTANT:
      return {ICON_GP_DRAW_FILL, paint_glyph::FILL};
    case MA_PAINT_LAYER_SOURCE_MATERIAL:
      return {ICON_MATERIAL, paint_glyph::TEXTURE};
    case MA_PAINT_LAYER_SOURCE_GENERATOR:
      /* A pattern wears its own glyph so it is not read as a Gradient; the Gradient, and a subtype
       * the caller does not name, keep the ramp. */
      if (!ELEM(subtype, 0, MA_PAINT_LAYER_GENERATOR_GRADIENT)) {
        return {ICON_TEXTURE, paint_glyph::PATTERN};
      }
      return {PAINT_GRADIENT_ICON, paint_glyph::GRADIENT};
    case MA_PAINT_LAYER_SOURCE_FILTER:
      return paint_filter_look(subtype);
    case MA_PAINT_LAYER_SOURCE_CURVE_PATCH:
      return {ICON_CURVE_DATA, 0};
    case MA_PAINT_LAYER_SOURCE_TEXT:
      return {ICON_FONT_DATA, 0};
    case MA_PAINT_LAYER_SOURCE_ANCHOR_POINT:
      return {ICON_EMPTY_AXIS, paint_glyph::ANCHOR};
    case MA_PAINT_LAYER_SOURCE_STACK:
      return {ICON_FILE_FOLDER, 0};
    case MA_PAINT_LAYER_SOURCE_MESH_MAP:
      /* A geometry map of the object; the glyph keeps the texture look, the icon names the mesh
       * the map is baked from. */
      return {ICON_MESH_DATA, paint_glyph::TEXTURE};
  }
  return {ICON_IMAGE_RGB, 0};
}

/** The look of an Anchor Point marker, whichever list it sits in. */
PaintKindLook paint_look_for_anchor_point()
{
  return {ICON_EMPTY_AXIS, paint_glyph::ANCHOR};
}

/** The look of an Anchor Point reader: what it lays, not the marker it reads. */
PaintKindLook paint_look_for_anchor_reader()
{
  return {ICON_LINKED, paint_glyph::CELL_TOWER};
}

static const PaintFilterAddKind paint_filter_add_kinds_table[] = {
    {"FILTER_INVERT",
     N_("Invert"),
     "Invert the active layer's colour, by the filter's opacity",
     MA_PAINT_LAYER_FILTER_INVERT},
    {"FILTER_LEVELS",
     N_("Levels"),
     "Remap the active layer's tones: black and white points, gamma and output range",
     MA_PAINT_LAYER_FILTER_LEVELS},
    {"FILTER_HSL",
     N_("Hue Saturation Value"),
     "Shift the active layer's hue, saturation and value",
     MA_PAINT_LAYER_FILTER_HSL},
    {"FILTER_BRIGHTNESS_CONTRAST",
     N_("Brightness/Contrast"),
     "Brighten or darken the active layer and scale its contrast",
     MA_PAINT_LAYER_FILTER_BRIGHTNESS_CONTRAST},
    {"FILTER_COLOR_RAMP",
     N_("Color Ramp"),
     "Remap the active layer's luminance through a color band",
     MA_PAINT_LAYER_FILTER_COLOR_RAMP},
    {"FILTER_POSTERIZE",
     N_("Posterize"),
     "Quantize the active layer's colours to a whole number of steps per channel",
     MA_PAINT_LAYER_FILTER_POSTERIZE},
    {"FILTER_MATH",
     N_("Math"),
     "Combine the active layer with a constant operand, per channel",
     MA_PAINT_LAYER_FILTER_MATH},
    {"FILTER_THRESHOLD",
     N_("Threshold"),
     "Turn the active layer's channels into the two states of a comparison",
     MA_PAINT_LAYER_FILTER_THRESHOLD},
    {"FILTER_CLAMP",
     N_("Clamp"),
     "Keep the active layer's channels inside a constant range",
     MA_PAINT_LAYER_FILTER_CLAMP},
    {"FILTER_HISTOGRAM_SCAN",
     N_("Histogram Scan"),
     "Remap the active layer's channels through a smoothstep: a soft threshold the contrast "
     "opens into a ramp, the one that grows and shrinks a mask",
     MA_PAINT_LAYER_FILTER_HISTOGRAM_SCAN},
    {"FILTER_HISTOGRAM_SHIFT",
     N_("Histogram Shift"),
     "Roll the tonal range of the active layer's channels by one amount, wrapping back through "
     "zero",
     MA_PAINT_LAYER_FILTER_HISTOGRAM_SHIFT},
    {"FILTER_SMOOTHSTEP",
     N_("Smoothstep"),
     "Extract a range of the active layer's grey between a minimum and a maximum: the Hermite "
     "interpolation between the edges, the smoother sibling of a threshold",
     MA_PAINT_LAYER_FILTER_SMOOTHSTEP},
    {"FILTER_CURVES",
     N_("RGB Curves"),
     "Remap the active layer's channels through their own curves",
     MA_PAINT_LAYER_FILTER_CURVES},
    {"FILTER_COLOR_BALANCE",
     N_("Color Balance"),
     "Balance the active layer's colours through lift, gamma and gain (or slope, offset and power)",
     MA_PAINT_LAYER_FILTER_COLOR_BALANCE},
    {"FILTER_EXPOSURE",
     N_("Exposure"),
     "Scale the active layer's colours by a power of two, shift them and raise the result to a gamma",
     MA_PAINT_LAYER_FILTER_EXPOSURE},
    {"FILTER_DEBAND",
     N_("Deband / Dither"),
     "Break the active layer's bands up with a triangular noise, or round its colours to fewer bits",
     MA_PAINT_LAYER_FILTER_DEBAND},
    {"FILTER_NORMAL_CONVERT",
     N_("Normal Convert"),
     "Convert the active layer's normal encoding: flip the green, rebuild or clear the blue, or "
     "move the channels between linear and sRGB",
     MA_PAINT_LAYER_FILTER_NORMAL_CONVERT},
    {"FILTER_CHANNEL_MIXER",
     N_("Channel Mixer"),
     "Mix the active layer's colour channels: each output is a weighted sum of the inputs plus a "
     "constant",
     MA_PAINT_LAYER_FILTER_CHANNEL_MIXER},
};

/**
 * The CPU-computed filters the Add Effect menu offers: either they read a neighbourhood of the
 * row's map (a Blur) or their sequence is not the graph's (a Noise), so they exist only as
 * corrections of a row's color -- the effect cache computes them and the row is baked for the
 * live graph. No adjustment or mask form is offered, because neither has a map of the row's to
 * read.
 */
static const PaintFilterAddKind paint_effect_add_kinds_table[] = {
    {"EFFECT_BLUR",
     N_("Blur"),
     "Blur the active layer's colours with a box or gaussian kernel",
     MA_PAINT_LAYER_FILTER_BLUR},
    {"EFFECT_BLUR_DIRECTIONAL",
     N_("Blur Directional"),
     "Streak the active layer's colours along a fixed angle: a line average the radius reaches "
     "along, mixed over the colour by its intensity",
     MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL},
    {"EFFECT_SHARPEN",
     N_("Sharpen"),
     "Sharpen the active layer's colours around the details a blur removes",
     MA_PAINT_LAYER_FILTER_SHARPEN},
    {"EFFECT_HIGH_PASS",
     N_("High Pass"),
     "Keep the active layer's details above a blurred copy of them",
     MA_PAINT_LAYER_FILTER_HIGH_PASS},
    {"EFFECT_EDGE_DETECT",
     N_("Edge Detect"),
     "Outline the active layer's edges with the Sobel gradient magnitude",
     MA_PAINT_LAYER_FILTER_EDGE_DETECT},
    {"EFFECT_EMBOSS",
     N_("Emboss"),
     "Emboss the active layer's colours with a diagonal lighting kernel",
     MA_PAINT_LAYER_FILTER_EMBOSS},
    {"EFFECT_ERODE_DILATE",
     N_("Erode / Dilate"),
     "Shrink or grow the bright regions of the active layer by the kernel's radius",
     MA_PAINT_LAYER_FILTER_ERODE_DILATE},
    {"EFFECT_NOISE",
     N_("Noise"),
     "Lay a grain (gaussian, uniform, speckle or salt and pepper) over the active layer's colours",
     MA_PAINT_LAYER_FILTER_NOISE},
    {"EFFECT_GLOW",
     N_("Glow"),
     "Bleed a coloured glow off the active layer's own light",
     MA_PAINT_LAYER_FILTER_GLOW},
    {"EFFECT_BEVEL",
     N_("Bevel"),
     "Lay a distance ramp from the borders of the active layer's content inward",
     MA_PAINT_LAYER_FILTER_BEVEL},
    {"EFFECT_COLOR_MATCH",
     N_("Color Match"),
     "Grade the active layer's colours from a source colour to a target one",
     MA_PAINT_LAYER_FILTER_COLOR_MATCH},
    {"EFFECT_DROP_SHADOW",
     N_("Drop Shadow"),
     "Cast a shadow off the active layer's shape: the silhouette spread, blurred and offset by "
     "the angle and the distance, tinted by the colour",
     MA_PAINT_LAYER_FILTER_DROP_SHADOW},
    {"EFFECT_MASK_OUTLINE",
     N_("Mask Outline"),
     "Lay a band along the border of the active layer's grey: inside it, outside it, or on both "
     "sides, the width the band takes and the shape its falloff walks",
     MA_PAINT_LAYER_FILTER_MASK_OUTLINE},
    {"EFFECT_FILL_AREA",
     N_("Fill Area"),
     "Fill what a continuous border of the active layer's content encloses: the regions the "
     "border closes off take the colour the border carries, diffused inward, or the solid colour",
     MA_PAINT_LAYER_FILTER_FILL_AREA},
    {"EFFECT_HEIGHT_TO_NORMAL",
     N_("Height To Normal"),
     "Convert the Height the active layer lays into a tangent-space normal: the Sobel gradients "
     "of the height, scaled by the intensity or the world units, encoded into the Normal channel",
     MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL},
};

/**
 * The map-driven filters the Add Effect menu offers over the mask. They only exist in a mask (their
 * role is a mask item), so they have no correction or adjustment form like #paint_filter_add_kinds.
 */
static const PaintFilterAddKind paint_coverage_add_kinds_table[] = {
    {"FILTER_COLOR_ID",
     N_("Color ID"),
     "Keep the active layer's mask only where an ID Map matches the listed colors",
     MA_PAINT_LAYER_FILTER_COLOR_ID},
    {"FILTER_UV_ISLAND",
     N_("UV Island"),
     "Keep the active layer's mask only on the UV islands captured from the Image Editor",
     MA_PAINT_LAYER_FILTER_UV_ISLAND},
};

/**
 * Whether a filter is listed in the "Filter" submenu of the Add Effect menu instead of the main
 * list. The submenu holds the filters added after the original set, so the main list keeps the
 * place the older ones have always had; a filter not named here stays in the main list rather than
 * disappearing from the menu.
 */
bool paint_filter_is_new(const int16_t filter)
{
  /* A switch rather than ELEM: the list outgrew the macro's argument count, and a new filter the
   * submenu names is one more case here. */
  switch (eMaterialPaintLayerFilter(filter)) {
    case MA_PAINT_LAYER_FILTER_SHARPEN:
    case MA_PAINT_LAYER_FILTER_HIGH_PASS:
    case MA_PAINT_LAYER_FILTER_EDGE_DETECT:
    case MA_PAINT_LAYER_FILTER_EMBOSS:
    case MA_PAINT_LAYER_FILTER_ERODE_DILATE:
    case MA_PAINT_LAYER_FILTER_EXPOSURE:
    case MA_PAINT_LAYER_FILTER_DEBAND:
    case MA_PAINT_LAYER_FILTER_NORMAL_CONVERT:
    case MA_PAINT_LAYER_FILTER_CHANNEL_MIXER:
    case MA_PAINT_LAYER_FILTER_NOISE:
    case MA_PAINT_LAYER_FILTER_GLOW:
    case MA_PAINT_LAYER_FILTER_BEVEL:
    case MA_PAINT_LAYER_FILTER_COLOR_MATCH:
      return true;
    default:
      return false;
  }
}

/**
 * Whether a filter is listed in the "Filter 2" submenu: the Painter-parity set, which starts at
 * the Histogram Scan and runs on in the enum's order, so a filter added after them lands here
 * without another case. A filter of an older set must not be given a value past it.
 */
bool paint_filter_is_filter2(const int16_t filter)
{
  return filter >= MA_PAINT_LAYER_FILTER_HISTOGRAM_SCAN;
}

/** The transform effects the Add Effect menu lists under "Transform" instead of the other lists. */
bool paint_filter_is_transform(const int16_t filter)
{
  return ELEM(eMaterialPaintLayerFilter(filter),
              MA_PAINT_LAYER_FILTER_TRANSFORM,
              MA_PAINT_LAYER_FILTER_MIRROR);
}

bool paint_filter_is_content_only(const int16_t filter)
{
  return filter == MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL;
}

Span<const PaintFilterAddKind> paint_filter_add_kinds()
{
  return Span<const PaintFilterAddKind>(paint_filter_add_kinds_table,
                                        ARRAY_SIZE(paint_filter_add_kinds_table));
}

Span<const PaintFilterAddKind> paint_effect_add_kinds()
{
  return Span<const PaintFilterAddKind>(paint_effect_add_kinds_table,
                                        ARRAY_SIZE(paint_effect_add_kinds_table));
}

Span<const PaintFilterAddKind> paint_coverage_add_kinds()
{
  return Span<const PaintFilterAddKind>(paint_coverage_add_kinds_table,
                                        ARRAY_SIZE(paint_coverage_add_kinds_table));
}

}  // namespace blender::ed::outliner