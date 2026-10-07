/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


/** \file
 * Paint Layers graph-vs-CPU tests: full-stack reference, presets and values sync (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_graph_eval_test_fixture.hh` /
 * `paint_layers_graph_eval_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_graph_eval_test_fixture.hh"
#include "intern/paint_layers_graph_eval_test_shared.hh"

namespace blender::bke::tests {

static const float kFsCOpacity = 0.4f;
static const float kFsFillColor[4] = {0.30f, 0.45f, 0.20f, 1.0f};
static const float kFsFillRoughness = 0.75f;
static const float kFsTopColor[4] = {0.60f, 0.80f, 0.50f, 1.0f};
static const float kFsTopRoughness[4] = {0.40f, 0.40f, 0.40f, 1.0f};

struct FsPoint {
  const char *label;
  int x;
  int y;
};
/* The mask grey is 1.0, 0.4, 0.8 and 0.0 at these texels; Material B's alpha is 0.6 at all. */
static const FsPoint kFsPoints[] = {
    {"center", 1, 1}, {"mask-edge", 1, 0}, {"b-partial-alpha", 0, 1}, {"mask-closed", 0, 0}};

/** Which parts of the stack take part in the reference. */
struct FsShape {
  bool iso = true;
  bool c = true;
  /** The row opacities a test drives through the RNA slider; the defaults are the stack's own. */
  float fill_opacity = 1.0f;
  float a_opacity = 1.0f;
  float b_opacity = 1.0f;
  float c_opacity = kFsCOpacity;
  float top_opacity = kFsTopOpacity;
};

/** One row's contribution to a channel: its colour and the factor before the row's opacity. */
struct FsRow {
  bool part = false;
  RGBA color;
  float factor = 0.0f;
};

static FsRow fs_row_fill(const int channel)
{
  FsRow row;
  if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    row = {true, {kFsFillColor[0], kFsFillColor[1], kFsFillColor[2], 1.0f}, 1.0f};
  }
  else if (channel == PAINT_MATERIAL_CHANNEL_ROUGHNESS) {
    row = {true, {kFsFillRoughness, kFsFillRoughness, kFsFillRoughness, 1.0f}, 1.0f};
  }
  return row;
}

/**
 * Material A: the source's clean value; on Base Color the Paint correction mixed in with
 * `opacity * alpha` (`paint_material_composite.cc:628-632`); the factor is the source coverage (one)
 * with the mask item laid over it as `F * (1 - fac) + C * fac`, `fac = alpha * opacity`
 * (`paint_material_composite.cc:366-396`, the same expression at `:664-690`). The mask is one for
 * every channel (`paint_layers_intern.hh:123`).
 */
static FsRow fs_row_a(const int channel, const int x, const int y)
{
  FsRow row;
  RGBA raw;
  if (!group_source_expected(source_spec_a, channel, x, y, raw)) {
    return row;
  }
  const int texel = y * kFsSize + x;
  if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    const float alpha = float(kFsCorrBytes[texel][3]) / 255.0f;
    float dst[4] = {raw.r, raw.g, raw.b, 1.0f};
    const float top[4] = {float(kFsCorrBytes[texel][0]) / 255.0f,
                          float(kFsCorrBytes[texel][1]) / 255.0f,
                          float(kFsCorrBytes[texel][2]) / 255.0f,
                          alpha};
    ramp_blend(MA_RAMP_BLEND, dst, clamp_f(kFsCorrOpacity * alpha, 0.0f, 1.0f), top);
    raw = {dst[0], dst[1], dst[2], 1.0f};
  }
  const float gray = float(kFsMaskBytes[texel]) / 255.0f;
  const float fac = clamp_f(kFsMaskOpacity, 0.0f, 1.0f);
  row.part = true;
  row.color = raw;
  row.factor = 1.0f * (1.0f - fac) + gray * fac;
  return row;
}

/** Material B: the constants of #source_spec_b, covered by its constant alpha. */
static FsRow fs_row_b(const int channel)
{
  FsRow row;
  RGBA raw;
  if (hybrid_source_expected(source_spec_b, channel, raw)) {
    row = {true, raw, source_spec_b.alpha};
  }
  return row;
}

/** The Paint row inside the isolating folder: Base Color only, fully covering. */
static FsRow fs_row_inner_paint(const int channel)
{
  FsRow row;
  if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    row = {true, {kIsolatingPaintA[0], kIsolatingPaintA[1], kIsolatingPaintA[2], 1.0f}, 1.0f};
  }
  return row;
}

static FsRow fs_row_c(const int channel, const int x, const int y)
{
  FsRow row;
  RGBA raw;
  if (group_source_expected(source_spec_c, channel, x, y, raw)) {
    row = {true, raw, 1.0f};
  }
  return row;
}

/** The top Paint row: Base Color and Roughness maps, fully covering. */
static FsRow fs_row_top(const int channel)
{
  FsRow row;
  if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    row = {true, {kFsTopColor[0], kFsTopColor[1], kFsTopColor[2], 1.0f}, 1.0f};
  }
  else if (channel == PAINT_MATERIAL_CHANNEL_ROUGHNESS) {
    row = {true, {kFsTopRoughness[0], kFsTopRoughness[1], kFsTopRoughness[2], 1.0f}, 1.0f};
  }
  return row;
}

/**
 * The whole stack over the channel bottom. A row lays its colour with `opacity * factor`
 * (`composite_apply_layer_linear`, `paint_material_composite.cc:777-804`, `blend_row_linear`
 * `:253-277`, `ramp_blend` `material.cc:2539-2549`); the isolating folder accumulates its children
 * premultiplied first (`composite_folder_accumulate`, `:702-775`) and its own coverage is that
 * accumulation; the Pass Through folder is transparent to the maths, so C is laid like a root row.
 * A row a channel does not exist for (Unavailable in the source) does not take part. The result
 * ends with the Normal decode-normalize-encode of `cpu_pixel_at` and of the chain
 * (`paint_layers.cc:542-570`).
 *
 * \return false when no row takes part in \a channel.
 */
static bool full_stack_expected(
    const int channel, const int x, const int y, const FsShape &shape, RGBA &r_out)
{
  const bool normal = (channel == PAINT_MATERIAL_CHANNEL_NORMAL);
  RGBA state = channel_bottom(channel);
  bool any = false;

  auto lay = [&](const FsRow &row, const float opacity, const int blend) {
    if (!row.part) {
      return;
    }
    any = true;
    blend_over_rgba(state, row.color, blend, opacity * row.factor, normal);
  };

  lay(fs_row_fill(channel), shape.fill_opacity, MA_RAMP_BLEND);
  lay(fs_row_a(channel, x, y), shape.a_opacity, MA_RAMP_BLEND);

  if (shape.iso) {
    struct Child {
      FsRow row;
      float opacity;
    };
    const Child children[2] = {{fs_row_b(channel), shape.b_opacity},
                               {fs_row_inner_paint(channel), kFsInnerPaintOpacity}};
    float premul[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float coverage = 0.0f;
    bool folder_part = false;
    for (const Child &child : children) {
      if (!child.row.part) {
        continue;
      }
      folder_part = true;
      const float f = clamp_f(child.opacity * child.row.factor, 0.0f, 1.0f);
      const float a = coverage;
      RGBA blended = {0.0f, 0.0f, 0.0f, 0.0f};
      if (a > 0.0f) {
        blended = {premul[0] / a, premul[1] / a, premul[2] / a, premul[3] / a};
      }
      blend_over_rgba(blended, child.row.color, MA_RAMP_BLEND, 1.0f, normal);
      const float c[4] = {
          child.row.color.r, child.row.color.g, child.row.color.b, child.row.color.a};
      const float b[4] = {blended.r, blended.g, blended.b, blended.a};
      for (const int k : IndexRange(4)) {
        const float c_eff = c[k] + (b[k] - c[k]) * a;
        premul[k] = premul[k] * (1.0f - f) + c_eff * f;
      }
      coverage = a + f * (1.0f - a);
    }
    if (folder_part) {
      any = true;
      if (coverage > 0.0f) {
        const RGBA folder_src = {premul[0] / coverage,
                                 premul[1] / coverage,
                                 premul[2] / coverage,
                                 premul[3] / coverage};
        blend_over_rgba(state, folder_src, MA_RAMP_BLEND, kFsIsoOpacity * coverage, normal);
      }
    }
  }

  if (shape.c) {
    lay(fs_row_c(channel, x, y), shape.c_opacity, MA_RAMP_BLEND);
  }
  lay(fs_row_top(channel), shape.top_opacity, MA_RAMP_MULT);

  if (normal) {
    float n[3] = {state.r * 2.0f - 1.0f, state.g * 2.0f - 1.0f, state.b * 2.0f - 1.0f};
    normalize_v3(n);
    state.r = n[0] * 0.5f + 0.5f;
    state.g = n[1] * 0.5f + 0.5f;
    state.b = n[2] * 0.5f + 0.5f;
  }
  r_out = state;
  return any;
}

/**
 * The mode a Material row must report, from the rule of `BKE_paint_layers_material_mode`
 * (`paint_layers_bake.cc:902-939`) and not from a run. Only the active row (or one of its
 * corrections or its mask) is live; its channels then decide: A and C map a texture through a Color
 * Ramp / mix, so SourceGroup; B is all constants and one flat texture, so Hybrid. Every other row
 * has a valid bake for each channel and reads Baked.
 * \a row: 0 = A, 1 = B, 2 = C. \a owner: the Material row the active item belongs to, or -1.
 */
static PaintLayerMaterialMode fs_expected_mode(const int row, const int owner)
{
  if (row != owner) {
    return PaintLayerMaterialMode::Baked;
  }
  return (row == 1) ? PaintLayerMaterialMode::Hybrid : PaintLayerMaterialMode::SourceGroup;
}

static void fs_expect_rgb(const RGBA &expected,
                          const RGBA &got,
                          const char *tag,
                          const char *channel,
                          const char *point,
                          const char *side)
{
  const float tolerance = 1e-4f;
  if (std::fabs(expected.r - got.r) <= tolerance && std::fabs(expected.g - got.g) <= tolerance &&
      std::fabs(expected.b - got.b) <= tolerance)
  {
    return;
  }
  ADD_FAILURE() << tag << " | " << channel << " | " << point << " | " << side << ": expected ("
                << expected.r << ", " << expected.g << ", " << expected.b << ") got (" << got.r
                << ", " << got.g << ", " << got.b << ")";
}

struct FsActiveCase {
  std::string label;
  bUUID marker;
  /** The Material row (0 = A, 1 = B, 2 = C) the active item is or belongs to, or -1. */
  int owner;
};

class PaintLayersFullStackTest : public PaintLayersGraphEvalTest {
 public:
  struct Options {
    bool iso = true;
    bool with_c = true;
    bool c_in_pass_folder = true;
  };

  struct Stack {
    Material *ma = nullptr;
    MaterialPaintLayer *fill = nullptr;
    MaterialPaintLayer *a = nullptr;
    MaterialPaintLayer *a_correction = nullptr;
    MaterialPaintLayer *a_mask = nullptr;
    MaterialPaintLayer *iso = nullptr;
    MaterialPaintLayer *b = nullptr;
    MaterialPaintLayer *inner_paint = nullptr;
    MaterialPaintLayer *pass = nullptr;
    MaterialPaintLayer *c = nullptr;
    MaterialPaintLayer *top = nullptr;
  };

  /** A 2x2 data-space byte map with straight per-texel RGBA, stored the way every paint map is. */
  Image *make_byte_map(const std::string &name, const uchar (*texels)[4])
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Image *image = BKE_image_add_generated(bmain,
                                           kFsSize,
                                           kFsSize,
                                           name.c_str(),
                                           32,
                                           false,
                                           IMA_GENTYPE_BLANK,
                                           color,
                                           false,
                                           true,
                                           false);
    if (image == nullptr) {
      return nullptr;
    }
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    image->flag |= IMA_GPU_LINEAR_PREMUL;
    make_image_data(image);
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    if (ibuf == nullptr) {
      return nullptr;
    }
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int i : IndexRange(kFsSize * kFsSize)) {
      for (const int k : IndexRange(4)) {
        pixels[i * 4 + k] = texels[i][k];
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  /** Bake \a row from the reference's own values, so the row can read Baked and stay valid. */
  bool bake_row(MaterialPaintLayer &row,
                const std::string &name,
                const bool grouped,
                const GroupSourceSpec &group_spec,
                const HybridSourceSpec &hybrid_spec,
                const float coverage)
  {
    auto raw_at = [&](const int channel, const int x, const int y, RGBA &r_out) -> bool {
      return grouped ? group_source_expected(group_spec, channel, x, y, r_out) :
                       hybrid_source_expected(hybrid_spec, channel, r_out);
    };
    for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
      RGBA probe;
      if (!raw_at(int(channel), 0, 0, probe)) {
        continue;
      }
      float pixels[4][3];
      for (const int y : IndexRange(kFsSize)) {
        for (const int x : IndexRange(kFsSize)) {
          RGBA raw;
          raw_at(int(channel), x, y, raw);
          pixels[y * kFsSize + x][0] = raw.r;
          pixels[y * kFsSize + x][1] = raw.g;
          pixels[y * kFsSize + x][2] = raw.b;
        }
      }
      const std::string map_name = name + " " + BKE_paint_material_channel_info(channel).ui_name;
      Image *map = make_data_pattern_map(bmain, map_name.c_str(), kFsSize, pixels);
      if (map == nullptr || !BKE_paint_layers_bake_set_map(*ma, row, int(channel), map)) {
        return false;
      }
    }
    const float coverage_rgba[4] = {coverage, coverage, coverage, 1.0f};
    Image *coverage_map = make_bake_data_map(
        bmain, (name + " Coverage").c_str(), kFsSize, coverage_rgba);
    if (coverage_map == nullptr || !BKE_paint_layers_bake_set_map(*ma, row, -1, coverage_map)) {
      return false;
    }
    BKE_paint_layers_bake_finalize(*ma, row);
    return BKE_paint_layers_bake_is_valid(*ma, row);
  }

  /** Build the stack; a null `Stack::ma` means a step failed. Leaves `ma` at the new material. */
  Stack build(const char *tag, const Options &options)
  {
    Stack s;
    const std::string t = tag;
    auto name = [&](const char *base) { return std::string(base) + t; };

    Material *source_a = build_group_source(*bmain,
                                            name("FsSrcA").c_str(),
                                            name("FsGrpA").c_str(),
                                            name("FsMapA").c_str(),
                                            name("FsShrA").c_str(),
                                            source_spec_a);
    Material *source_b = build_hybrid_source(
        *bmain, name("FsSrcB").c_str(), name("FsNrmB").c_str(), source_spec_b);
    Material *source_c = build_group_source(*bmain,
                                            name("FsSrcC").c_str(),
                                            name("FsGrpC").c_str(),
                                            name("FsMapC").c_str(),
                                            name("FsShrC").c_str(),
                                            source_spec_c);
    if (source_a == nullptr || source_b == nullptr || source_c == nullptr) {
      return s;
    }
    ma = BKE_material_add(bmain, name("FsStack").c_str());

    /* 1. Fill: Base Color from the fill colour, Roughness from its record. */
    s.fill = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
    if (s.fill == nullptr ||
        BKE_paint_layers_channel_add(*ma, s.fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR) == nullptr ||
        BKE_paint_layers_channel_add(*ma, s.fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS) == nullptr)
    {
      return s;
    }
    BKE_paint_layers_channel_set_value(*ma, s.fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, kFsFillColor);
    const float rough[4] = {kFsFillRoughness, kFsFillRoughness, kFsFillRoughness, 1.0f};
    BKE_paint_layers_channel_set_value(*ma, s.fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough);

    /* 2. Material A with a Paint correction and a mask. */
    s.a = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatA", nullptr, PaintLayerPlace::Above);
    if (s.a == nullptr || !BKE_paint_layers_set_material(*ma, s.a, source_a) ||
        !bake_row(*s.a, name("FsBakeA"), true, source_spec_a, {}, 1.0f))
    {
      return s;
    }
    Image *corr_map = make_byte_map(name("FsCorr"), kFsCorrBytes);
    const uchar mask_texels[4][4] = {{kFsMaskBytes[0], kFsMaskBytes[0], kFsMaskBytes[0], 255},
                                     {kFsMaskBytes[1], kFsMaskBytes[1], kFsMaskBytes[1], 255},
                                     {kFsMaskBytes[2], kFsMaskBytes[2], kFsMaskBytes[2], 255},
                                     {kFsMaskBytes[3], kFsMaskBytes[3], kFsMaskBytes[3], 255}};
    Image *mask_map = make_byte_map(name("FsMask"), mask_texels);
    if (corr_map == nullptr || mask_map == nullptr) {
      return s;
    }
    s.a_correction = add_content_correction(*ma, *s.a, corr_map);
    s.a_mask = set_mask_image(*s.a, mask_map);
    if (s.a_correction == nullptr || s.a_mask == nullptr) {
      return s;
    }
    BKE_paint_layers_set_opacity(*ma, s.a_correction, kFsCorrOpacity);
    /* The mask item starts as a Multiply constant; the reference lays it as a Mix. */
    BKE_paint_layers_set_blend(*ma, s.a_mask, MA_PAINT_LAYER_BLEND_MIX);
    BKE_paint_layers_set_opacity(*ma, s.a_mask, kFsMaskOpacity);

    /* 3. The isolating folder: Material B (Hybrid) and a Paint row. */
    if (options.iso) {
      s.iso = BKE_paint_layers_add(
          *ma, MA_PAINT_LAYER_SOURCE_STACK, "Iso", nullptr, PaintLayerPlace::Above);
      if (s.iso == nullptr) {
        return s;
      }
      BKE_paint_layers_set_opacity(*ma, s.iso, kFsIsoOpacity);
      s.b = BKE_paint_layers_add(
          *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatB", s.iso, PaintLayerPlace::Into);
      if (s.b == nullptr || !BKE_paint_layers_set_material(*ma, s.b, source_b) ||
          !bake_row(*s.b, name("FsBakeB"), false, source_spec_a, source_spec_b, source_spec_b.alpha))
      {
        return s;
      }
      s.inner_paint = BKE_paint_layers_add(
          *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "InnerPaint", s.iso, PaintLayerPlace::Into);
      if (s.inner_paint == nullptr) {
        return s;
      }
      MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
          *ma, s.inner_paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
      Image *inner_map = make_bake_data_map(
          bmain, name("FsInner").c_str(), kFsSize, kIsolatingPaintA);
      if (record == nullptr || inner_map == nullptr) {
        return s;
      }
      record->image = inner_map;
      record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
      BKE_paint_layers_set_opacity(*ma, s.inner_paint, kFsInnerPaintOpacity);
    }

    /* 4. Material C, in a Pass Through folder or straight in the root. */
    if (options.with_c) {
      if (options.c_in_pass_folder) {
        s.pass = BKE_paint_layers_add(
            *ma, MA_PAINT_LAYER_SOURCE_STACK, "Pass", nullptr, PaintLayerPlace::Above);
        if (s.pass == nullptr || !BKE_paint_layers_folder_is_pass_through(*ma, *s.pass)) {
          return s;
        }
      }
      s.c = BKE_paint_layers_add(*ma,
                                 MA_PAINT_LAYER_SOURCE_MATERIAL,
                                 "MatC",
                                 s.pass,
                                 s.pass != nullptr ? PaintLayerPlace::Into : PaintLayerPlace::Above);
      if (s.c == nullptr || !BKE_paint_layers_set_material(*ma, s.c, source_c) ||
          !bake_row(*s.c, name("FsBakeC"), true, source_spec_c, {}, 1.0f))
      {
        return s;
      }
      BKE_paint_layers_set_opacity(*ma, s.c, kFsCOpacity);
    }

    /* 5. The top Paint row: Multiply, opacity below one, Base Color and Roughness maps. */
    Image *top_color = make_bake_data_map(bmain, name("FsTopColor").c_str(), kFsSize, kFsTopColor);
    Image *top_rough = make_bake_data_map(
        bmain, name("FsTopRough").c_str(), kFsSize, kFsTopRoughness);
    if (top_color == nullptr || top_rough == nullptr) {
      return s;
    }
    s.top = add_layer("Top", MA_PAINT_LAYER_SOURCE_IMAGE, top_color);
    MaterialPaintLayerChannel *top_rough_record = BKE_paint_layers_channel_add(
        *ma, s.top, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
    if (s.top == nullptr || top_rough_record == nullptr) {
      return s;
    }
    top_rough_record->image = top_rough;
    top_rough_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    BKE_paint_layers_set_blend(*ma, s.top, MA_PAINT_LAYER_BLEND_MULTIPLY);
    BKE_paint_layers_set_opacity(*ma, s.top, kFsTopOpacity);

    /* The Material rows read Specular, Emission and the Alpha coverage, none of them in the build
     * default set; the reference model takes them into part. */
    channel_set_extend(*ma,
                       channel_bit(PAINT_MATERIAL_CHANNEL_SPECULAR) |
                           channel_bit(PAINT_MATERIAL_CHANNEL_EMISSION) |
                           channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA));

    s.ma = ma;
    return s;
  }

  Vector<FsActiveCase> active_cases(const Stack &s)
  {
    Vector<FsActiveCase> cases;
    cases.append({"none", BLI_uuid_nil(), -1});
    cases.append({"fill", s.fill->marker, -1});
    cases.append({"mat-a", s.a->marker, 0});
    cases.append({"a-correction", s.a_correction->marker, 0});
    cases.append({"a-mask", s.a_mask->marker, 0});
    if (s.iso != nullptr) {
      cases.append({"iso-folder", s.iso->marker, -1});
      cases.append({"mat-b", s.b->marker, 1});
      cases.append({"inner-paint", s.inner_paint->marker, -1});
    }
    if (s.pass != nullptr) {
      cases.append({"pass-folder", s.pass->marker, -1});
    }
    if (s.c != nullptr) {
      cases.append({"mat-c", s.c->marker, 2});
    }
    cases.append({"top", s.top->marker, -1});
    return cases;
  }

  /**
   * Compare the current `ma`'s generated graph and CPU composite with #full_stack_expected at every
   * channel and point. \a strict_wiring: a channel is wired exactly when a row takes part; off for a
   * stack whose hidden rows still wire their channels.
   */
  void check_values(const char *tag, const FsShape &shape, const bool strict_wiring)
  {
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    ASSERT_NE(interpreter.instance, nullptr) << tag;
    interpreter.tree->ensure_topology_cache();

    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      const int channel = int(info.channel);
      RGBA probe;
      const bool part = full_stack_expected(channel, 0, 0, shape, probe);
      const bool exists = result_output_exists(*ma->paint_layers_tree, channel);
      if ((strict_wiring && exists != part) || (part && !exists)) {
        ADD_FAILURE() << tag << " | " << info.ui_name << ": wired=" << exists
                      << " but a row takes part=" << part;
        continue;
      }
      if (!exists) {
        continue;
      }
      for (const FsPoint &point : kFsPoints) {
        RGBA expected;
        full_stack_expected(channel, point.x, point.y, shape, expected);
        interpreter.x = point.x;
        interpreter.y = point.y;
        fs_expect_rgb(expected,
                      eval_channel_result(interpreter, eMaterialPaintChannel(channel)),
                      tag,
                      info.ui_name,
                      point.label,
                      "graph");
        if (part) {
          fs_expect_rgb(expected,
                        cpu_pixel_at(channel, point.x, point.y),
                        tag,
                        info.ui_name,
                        point.label,
                        "cpu");
        }
      }
    }
  }
};

/**
 * Every active row, folder, correction and mask: the graph and the CPU equal the reference at every
 * channel and point, the report carries the mode and `deferred` the rule gives, and a second
 * regeneration rebuilds nothing.
 */
TEST_F(PaintLayersFullStackTest, every_active_row_matches_the_reference_and_the_cpu)
{
  const Stack s = build("All", {});
  ASSERT_NE(s.ma, nullptr);
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *s.iso));

  const MaterialPaintLayer *material_rows[3] = {s.a, s.b, s.c};
  for (const FsActiveCase &active : active_cases(s)) {
    const char *tag = active.label.c_str();
    BKE_paint_layers_active_set(*ma, active.marker);
    PaintLayersRegenerateReport report;
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report)) << tag;
    ASSERT_NE(ma->paint_layers_tree, nullptr) << tag;

    EXPECT_EQ(report.material_rows.size(), 3) << tag;
    for (const int row_index : IndexRange(3)) {
      const PaintLayersRegenerateReport::MaterialRowModeReport *found = nullptr;
      for (const auto &row : report.material_rows) {
        if (BLI_uuid_equal(row.marker, material_rows[row_index]->marker)) {
          found = &row;
        }
      }
      ASSERT_NE(found, nullptr) << tag << " row " << row_index;
      EXPECT_EQ(found->mode, fs_expected_mode(row_index, active.owner))
          << tag << " row " << row_index << " mode";
      EXPECT_EQ(found->deferred, row_index == active.owner)
          << tag << " row " << row_index << " deferred";
      EXPECT_EQ(found->refusal, PaintLayersSourceGroupRefusal::None) << tag << " row " << row_index;
    }

    check_values(tag, {}, true);

    /* The next regeneration, without an edit, must rebuild nothing. */
    bNodeTree *root = ma->paint_layers_tree;
    const Vector<bNode *> nodes = root_node_ptrs(*root);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
    EXPECT_EQ(ma->paint_layers_tree, root) << tag;
    EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root))) << tag << ": second regen rebuilt";
  }
}

/**
 * A second regeneration without edits rebuilds nothing, whichever row is active. The active row can
 * be inside the isolating folder: the root hash must not depend on whether that folder's group was
 * rebuilt in the same pass (paint_layers_root_topology_hash).
 */
TEST_F(PaintLayersFullStackTest, second_regen_after_an_active_change_rebuilds_nothing)
{
  const Stack s = build("Stable", {});
  ASSERT_NE(s.ma, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  for (const FsActiveCase &active : active_cases(s)) {
    const char *tag = active.label.c_str();
    BKE_paint_layers_active_set(*ma, active.marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
    bNodeTree *root = ma->paint_layers_tree;
    const Vector<bNode *> nodes = root_node_ptrs(*root);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
    EXPECT_EQ(ma->paint_layers_tree, root) << tag;
    EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root))) << tag << ": second regen rebuilt";
  }
}

/** The first regeneration of a fresh stack settles: the second one keeps the root. */
TEST_F(PaintLayersFullStackTest, initial_build_settles_in_one_regen)
{
  const Stack s = build("Initial", {});
  ASSERT_NE(s.ma, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> nodes = root_node_ptrs(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root))) << "second regen rebuilt";
}

/**
 * Hiding C and the isolating folder is a value edit (the root keeps its nodes) and gives the stack
 * without them; a stack built without them gives the same.
 */
TEST_F(PaintLayersFullStackTest, hiding_c_and_the_isolating_folder_equals_the_stack_without_them)
{
  const Stack full = build("Hide", {});
  ASSERT_NE(full.ma, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> nodes = root_node_ptrs(*root);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, full.c, false));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, full.iso, false));
  BKE_paint_layers_values_sync(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root))) << "hiding rebuilt the root";
  FsShape hidden;
  hidden.iso = false;
  hidden.c = false;
  check_values("hidden", hidden, false);

  Options bare_options;
  bare_options.iso = false;
  bare_options.with_c = false;
  const Stack bare = build("HideBare", bare_options);
  ASSERT_NE(bare.ma, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  check_values("built-without", hidden, true);
}

/**
 * Hiding the Pass Through folder hides C through the folder's visibility scale, a value edit: the
 * root keeps its nodes and the result is the stack without C, on the graph and on the CPU.
 */
TEST_F(PaintLayersFullStackTest, hiding_the_pass_through_folder_hides_c_as_a_value_edit)
{
  const Stack s = build("HidePass", {});
  ASSERT_NE(s.ma, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> nodes = root_node_ptrs(*root);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, s.pass, false));
  BKE_paint_layers_values_sync(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root))) << "hiding the folder rebuilt the root";
  FsShape without_c;
  without_c.c = false;
  check_values("pass-hidden", without_c, true);
}
/**
 * The other direction: a real structural edit still rebuilds what it must. A blend change shows in
 * the values, a new top-level row and a move into the isolating folder change the root's nodes.
 */
TEST_F(PaintLayersFullStackTest, real_structure_edits_still_rebuild)
{
  const Stack s = build("Edits", {});
  ASSERT_NE(s.ma, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  auto base_color = [&]() {
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    EXPECT_NE(interpreter.instance, nullptr);
    interpreter.tree->ensure_topology_cache();
    return eval_channel_result(interpreter, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  };

  /* A blend change rebuilds the row's group: the result moves. */
  const RGBA before_blend = base_color();
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, s.top, MA_PAINT_LAYER_BLEND_ADD));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const RGBA after_blend = base_color();
  EXPECT_GT(std::fabs(after_blend.r - before_blend.r) + std::fabs(after_blend.g - before_blend.g) +
                std::fabs(after_blend.b - before_blend.b),
            1e-3f);

  /* A new top-level row changes the root's nodes. */
  bNodeTree *root = ma->paint_layers_tree;
  Vector<bNode *> nodes = root_node_ptrs(*root);
  MaterialPaintLayer *extra = add_layer("Extra", MA_PAINT_LAYER_SOURCE_IMAGE,
                                        make_bake_data_map(bmain, "FsExtra", kFsSize, kFsTopColor));
  ASSERT_NE(extra, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(same_node_ptrs(nodes, root_node_ptrs(*ma->paint_layers_tree))) << "new row";

  /* Moving a root row into the isolating folder removes its instance from the root. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  nodes = root_node_ptrs(*ma->paint_layers_tree);
  ASSERT_TRUE(BKE_paint_layers_move(*ma, extra, s.iso, PaintLayerPlace::Into));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(same_node_ptrs(nodes, root_node_ptrs(*ma->paint_layers_tree))) << "move into folder";
}
/** The Pass Through folder is transparent: C directly in the root gives the same stack. */
TEST_F(PaintLayersFullStackTest, pass_through_folder_equals_c_in_the_root)
{
  Options flat_options;
  flat_options.c_in_pass_folder = false;
  const Stack folded = build("PassIn", {});
  const Stack flat = build("PassOut", flat_options);
  ASSERT_NE(folded.ma, nullptr);
  ASSERT_NE(flat.ma, nullptr);

  for (const Stack *stack : {&folded, &flat}) {
    ma = stack->ma;
    for (const bUUID &marker : {BLI_uuid_nil(), stack->c->marker}) {
      BKE_paint_layers_active_set(*ma, marker);
      ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
      check_values(stack == &folded ? "folded" : "flat", {}, true);
    }
  }
}

/**
 * One regeneration resolves each distinct source at most once, however many rows read it and however
 * many times the topology hashes, the wired set and the build ask about it. The resolver walks the
 * source's tree with its groups, so before the per-call cache one Material row cost dozens of walks.
 * The counter is a test hook: nothing else can tell "resolved once" from "resolved fast".
 */
TEST_F(PaintLayersFullStackTest, one_regeneration_resolves_each_source_at_most_once)
{
  const Stack s = build("Resolves", {});
  ASSERT_NE(s.ma, nullptr);
  /* A, B and C read three different sources. */
  const int64_t distinct_sources = 3;

  const int64_t first_before = BKE_paint_material_source_resolve_call_count();
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_LE(BKE_paint_material_source_resolve_call_count() - first_before, distinct_sources)
      << "initial build";

  for (const FsActiveCase &active : active_cases(s)) {
    const char *tag = active.label.c_str();
    BKE_paint_layers_active_set(*ma, active.marker);
    for (const int pass : IndexRange(2)) {
      const int64_t before = BKE_paint_material_source_resolve_call_count();
      ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
      EXPECT_LE(BKE_paint_material_source_resolve_call_count() - before, distinct_sources)
          << tag << " pass " << pass;
    }
  }
}

/** Every row's own group (`.PL Layer` / `.PL Folder`) with the nodes it holds, keyed by the tree. */
static Map<const bNodeTree *, Vector<bNode *>> row_group_snapshot(Main &bmain)
{
  Map<const bNodeTree *, Vector<bNode *>> snapshot;
  for (bNodeTree &group : bmain.nodetrees) {
    const char *name = group.id.name + 2;
    if (STRPREFIX(name, ".PL Layer") || STRPREFIX(name, ".PL Folder")) {
      snapshot.add(&group, root_node_ptrs(group));
    }
  }
  return snapshot;
}

/**
 * A regeneration without an edit rebuilds neither the root nor any row's group: the same trees
 * survive and each keeps its exact node list. The root-only check of the neighbouring tests would
 * miss a group whose hash drifted between two passes.
 */
TEST_F(PaintLayersFullStackTest, second_regen_rebuilds_no_row_group)
{
  const Stack s = build("Groups", {});
  ASSERT_NE(s.ma, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  for (const FsActiveCase &active : active_cases(s)) {
    const char *tag = active.label.c_str();
    BKE_paint_layers_active_set(*ma, active.marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
    bNodeTree *root = ma->paint_layers_tree;
    const Vector<bNode *> root_nodes = root_node_ptrs(*root);
    const Map<const bNodeTree *, Vector<bNode *>> before = row_group_snapshot(*bmain);
    ASSERT_FALSE(before.is_empty()) << tag;

    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
    EXPECT_EQ(ma->paint_layers_tree, root) << tag;
    EXPECT_TRUE(same_node_ptrs(root_nodes, root_node_ptrs(*root))) << tag << ": root rebuilt";
    const Map<const bNodeTree *, Vector<bNode *>> after = row_group_snapshot(*bmain);
    EXPECT_EQ(before.size(), after.size()) << tag << ": a row group appeared or vanished";
    for (const auto item : before.items()) {
      const Vector<bNode *> *nodes = after.lookup_ptr(item.key);
      if (nodes == nullptr) {
        ADD_FAILURE() << tag << ": row group " << item.key->id.name + 2 << " was replaced";
        continue;
      }
      EXPECT_TRUE(same_node_ptrs(item.value, *nodes))
          << tag << ": row group " << item.key->id.name + 2 << " was rebuilt";
    }
  }
}

/**
 * A Material row whose bake is still being rendered stays live, and a row whose bake has landed
 * goes to its maps: the maps in flight are the pending claim the editor's job code holds. While it
 * is claimed the row is not deferred (the user has moved on) yet shows its source, so the graph and
 * the CPU equal the reference at every point in every state, and a regeneration in any state
 * rebuilds nothing more.
 */
TEST_F(PaintLayersFullStackTest, rows_with_a_bake_in_flight_stay_live_until_it_lands)
{
  const Stack s = build("InFlight", {});
  ASSERT_NE(s.ma, nullptr);
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *s.iso));
  const MaterialPaintLayer *material_rows[3] = {s.a, s.b, s.c};

  Vector<uint32_t> claimed;
  for (const MaterialPaintLayer *row : material_rows) {
    ASSERT_NE(row->bake, nullptr);
    for (Image *image : row->bake->images) {
      if (image != nullptr) {
        claimed.append(image->id.session_uid);
      }
    }
    ASSERT_NE(row->bake->coverage, nullptr);
    claimed.append(row->bake->coverage->id.session_uid);
  }

  const auto row_groups_changed = [](const Map<const bNodeTree *, Vector<bNode *>> &before,
                                     const Map<const bNodeTree *, Vector<bNode *>> &after) {
    int changed = 0;
    for (const auto item : before.items()) {
      const Vector<bNode *> *nodes = after.lookup_ptr(item.key);
      if (nodes == nullptr || !same_node_ptrs(item.value, *nodes)) {
        changed++;
      }
    }
    return changed;
  };

  /* Nothing is being baked: every inactive row is on its maps, as before. */
  BKE_paint_layers_active_set(*ma, s.a->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  for (const int row_index : IndexRange(3)) {
    EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *material_rows[row_index]),
              fs_expected_mode(row_index, 0))
        << "row " << row_index;
  }

  /* The user moves off A: its bake is handed over and claimed. Only B, the new active row, changes
   * mode; A keeps its live graph, and nothing shows a mode between live and baked. */
  const Map<const bNodeTree *, Vector<bNode *>> before_switch = row_group_snapshot(*bmain);
  for (const uint32_t uid : claimed) {
    BKE_paint_layers_bake_image_pending_add(uid);
  }
  BKE_paint_layers_active_set(*ma, s.b->marker);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *s.a), PaintLayerMaterialMode::SourceGroup);
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *s.b), PaintLayerMaterialMode::Hybrid);
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *s.c), PaintLayerMaterialMode::SourceGroup);
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *s.a));
  EXPECT_FALSE(BKE_paint_layers_material_bake_ready(*ma, *s.a));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *s.a));
  check_values("in-flight", {}, true);
  /* A stays as it was; B (now active), its folder and C (its bake is in flight too) change, and
   * nothing else. */
  EXPECT_LE(row_groups_changed(before_switch, row_group_snapshot(*bmain)), 3);

  /* A second regeneration while the bake is in flight rebuilds nothing. */
  const Map<const bNodeTree *, Vector<bNode *>> settled = row_group_snapshot(*bmain);
  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_node_ptrs(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(root_before, root_node_ptrs(*root)));
  EXPECT_EQ(row_groups_changed(settled, row_group_snapshot(*bmain)), 0);

  /* The maps land: A and C go to them in one regeneration, B stays live as the active row. */
  for (const uint32_t uid : claimed) {
    BKE_paint_layers_bake_image_pending_remove(uid);
  }
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  for (const int row_index : IndexRange(3)) {
    EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *material_rows[row_index]),
              fs_expected_mode(row_index, 1))
        << "row " << row_index;
  }
  EXPECT_TRUE(BKE_paint_layers_material_bake_ready(*ma, *s.a));
  check_values("landed", {}, true);

  const Map<const bNodeTree *, Vector<bNode *>> landed = row_group_snapshot(*bmain);
  root = ma->paint_layers_tree;
  const Vector<bNode *> landed_root = root_node_ptrs(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(landed_root, root_node_ptrs(*root)));
  EXPECT_EQ(row_groups_changed(landed, row_group_snapshot(*bmain)), 0);
}

/**
 * Moving the active row back and forth over a row whose bake is valid and complete is one mode
 * change each way and never a mode in between: with nothing to bake there is no reason for the row
 * to be anything but live while active and baked otherwise.
 */
TEST_F(PaintLayersFullStackTest, moving_the_active_row_over_a_baked_row_never_passes_hybrid)
{
  const Stack s = build("Toggle", {});
  ASSERT_NE(s.ma, nullptr);
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *s.iso));
  for (const int round : IndexRange(3)) {
    BKE_paint_layers_active_set(*ma, s.a->marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << round;
    EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *s.a), PaintLayerMaterialMode::SourceGroup);
    BKE_paint_layers_active_set(*ma, s.top->marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << round;
    EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *s.a), PaintLayerMaterialMode::Baked);
    check_values("toggle", {}, true);
  }
}

/**
 * Moving a row's opacity through the BKE setter across 1.0 (1 -> 0.7 -> 1 -> 0.3) is a value edit:
 * no row group and not the root is rebuilt, and the graph follows the formula. Covers a Hybrid row
 * (B), SourceGroup rows (A, C, live while active), a Paint row and a Fill row. The RNA slider
 * forwards to the same setter (rna_material.cc); its owner resolution is covered by the RNA suite.
 */
TEST_F(PaintLayersFullStackTest, opacity_across_one_through_bke_rebuilds_nothing)
{
  const Stack s = build("OpacityRna", {});
  ASSERT_NE(s.ma, nullptr);

  struct Target {
    const char *label;
    MaterialPaintLayer *row;
    float FsShape::*member;
    bool make_active;
  };
  const Target targets[] = {{"fill", s.fill, &FsShape::fill_opacity, false},
                            {"mat-a-sourcegroup", s.a, &FsShape::a_opacity, true},
                            {"mat-b-hybrid", s.b, &FsShape::b_opacity, true},
                            {"mat-c-sourcegroup", s.c, &FsShape::c_opacity, true},
                            {"top-paint", s.top, &FsShape::top_opacity, false}};

  for (const Target &target : targets) {
    BKE_paint_layers_active_set(*ma, target.make_active ? target.row->marker : BLI_uuid_nil());
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << target.label;
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << target.label;

    /* Each target starts from the stack's own opacity, so the shape it is checked against is the
     * default one and the earlier targets' last value (0.3) cannot leak in. */
    FsShape shape;
    {
      ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, target.row, shape.*(target.member)));
      ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << target.label;
    }
    for (const float opacity : {1.0f, 0.7f, 1.0f, 0.3f}) {
      const std::string tag = std::string(target.label) + " @" + std::to_string(opacity);
      bNodeTree *root = ma->paint_layers_tree;
      const Vector<bNode *> root_nodes = root_node_ptrs(*root);
      const Map<const bNodeTree *, Vector<bNode *>> before = row_group_snapshot(*bmain);

      ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, target.row, opacity));
      shape.*(target.member) = opacity;

      ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
      EXPECT_EQ(ma->paint_layers_tree, root) << tag << ": root replaced";
      EXPECT_TRUE(same_node_ptrs(root_nodes, root_node_ptrs(*root))) << tag << ": root rebuilt";
      const Map<const bNodeTree *, Vector<bNode *>> after = row_group_snapshot(*bmain);
      EXPECT_EQ(before.size(), after.size()) << tag << ": a row group appeared or vanished";
      for (const auto item : before.items()) {
        const Vector<bNode *> *nodes = after.lookup_ptr(item.key);
        if (nodes == nullptr) {
          ADD_FAILURE() << tag << ": row group " << item.key->id.name + 2 << " was replaced";
          continue;
        }
        EXPECT_TRUE(same_node_ptrs(item.value, *nodes))
            << tag << ": row group " << item.key->id.name + 2 << " was rebuilt";
      }
      check_values(tag.c_str(), shape, true);
    }
    /* Back to the stack's own opacity, so the next target is checked against the default shape. */
    BKE_paint_layers_set_opacity(*ma, target.row, FsShape().*(target.member));
  }
}

/**
 * One #BKE_paint_layers_values_sync resolves each source at most once (in fact not at all: values
 * come from the description), and leaves the graph at the reference.
 */
TEST_F(PaintLayersFullStackTest, values_sync_resolves_each_source_at_most_once)
{
  const Stack s = build("SyncResolves", {});
  ASSERT_NE(s.ma, nullptr);
  const int64_t distinct_sources = 3;
  for (const FsActiveCase &active : active_cases(s)) {
    const char *tag = active.label.c_str();
    BKE_paint_layers_active_set(*ma, active.marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
    const int64_t before = BKE_paint_material_source_resolve_call_count();
    BKE_paint_layers_values_sync(*ma);
    EXPECT_LE(BKE_paint_material_source_resolve_call_count() - before, distinct_sources) << tag;
    check_values(tag, {}, true);
  }
}

/** \} */

}  // namespace blender::bke::tests
