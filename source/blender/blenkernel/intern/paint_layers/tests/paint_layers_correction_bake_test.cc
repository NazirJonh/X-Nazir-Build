/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Tests for #BKE_paint_layers_corrections_bake: the merge of Paint, Fill and Gradient corrections
 * into one Paint correction, the surface an Object or a Generated Gradient line reads, and the
 * refusals that stay.
 *
 * The comparison is against #BKE_paint_material_composite_eval_row_content of the same
 * corrections, decoded from the merged map's byte buffer the way the composite reads it back; the
 * absolute expectations (a UV line across the unit square is grey `(x + 0.5) / size` at texel x)
 * guard against a wrong description that both sides would then share.
 */

#include "testing/testing.h"

#include <memory>

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_gradient_surface.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_material_composite.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "../paint_layers_intern.hh"
#include "paint_layers_test_util.hh"

namespace blender::bke::tests {

class PaintLayersCorrectionBakeTest : public PaintLayersTestBase {
 public:
  Object *surface_object = nullptr;
  Mesh *surface_mesh = nullptr;

  void SetUp() override
  {
    PaintLayersTestBase::SetUp();
    /* The canvas follows the corrections' maps; a material size would pin it. */
    ma->paint_layers_image_size = 0;
  }

  /** A blank opaque-black byte image of \a size x \a size, optionally UDIM-tiled (tile 1001). */
  Image *add_byte_image(const char *name, const int size, const bool tiled = false)
  {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, black, false, false, tiled);
    EXPECT_NE(image, nullptr);
    return image;
  }

  /** A Paint correction with \a map on Base Color, on top of \a owner. */
  MaterialPaintLayer *add_paint_correction(MaterialPaintLayer &owner,
                                           const char *name,
                                           Image *map)
  {
    MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
        *ma, &owner, name, MA_PAINT_LAYER_SOURCE_IMAGE);
    EXPECT_NE(effect, nullptr);
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
              nullptr);
    EXPECT_TRUE(
        BKE_paint_layers_channel_set_image(*ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
    return effect;
  }

  /** A Fill correction laying the opaque \a color at \a opacity, on top of \a owner. */
  MaterialPaintLayer *add_fill_correction(MaterialPaintLayer &owner,
                                          const char *name,
                                          const float color[4],
                                          const float opacity)
  {
    MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
        *ma, &owner, name, MA_PAINT_LAYER_SOURCE_CONSTANT);
    EXPECT_NE(effect, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_fill_color(*ma, effect, color));
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, effect, opacity));
    return effect;
  }

  /** A Gradient correction in \a space with the line \a start to \a end, on top of \a owner. */
  MaterialPaintLayer *add_gradient_correction(MaterialPaintLayer &owner,
                                              const char *name,
                                              const eMaterialPaintLayerGradientSpace space,
                                              const float start[3],
                                              const float end[3])
  {
    MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
        *ma, &owner, name, MA_PAINT_LAYER_SOURCE_GENERATOR);
    EXPECT_NE(effect, nullptr);
    EXPECT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *effect, space));
    EXPECT_TRUE(BKE_paint_layers_gradient_set_line(*ma, *effect, start, end));
    return effect;
  }

  /**
   * A 2x2 quad in the X/Y plane whose UVs cover \a uv_min..\a uv_max of the unit square, so an
   * island smaller than the square leaves the texels around it empty. The object position of UV
   * (u, v) is (2u, 2v, 0), and with no orco layer the generated coordinate is the texspace box's
   * (u, v, 0.5). The custom texspace pins the box the shader would compute; the default UV map
   * name is set the way `Texture Coordinate.UV` reads it.
   */
  void create_surface_quad(const float uv_min = 0.0f, const float uv_max = 1.0f)
  {
    surface_object = BKE_object_add_only_object(bmain, OB_MESH, "BakeSurface");
    surface_mesh = BKE_mesh_add(bmain, "BakeSurfaceMesh");
    surface_object->data = &surface_mesh->id;

    Mesh *source = BKE_mesh_new_nomain(4, 0, 1, 4);
    MutableSpan<float3> positions = source->vert_positions_for_write();
    positions[0] = float3(0.0f, 0.0f, 0.0f);
    positions[1] = float3(2.0f, 0.0f, 0.0f);
    positions[2] = float3(2.0f, 2.0f, 0.0f);
    positions[3] = float3(0.0f, 2.0f, 0.0f);
    MutableSpan<int> face_offsets = source->face_offsets_for_write();
    face_offsets[0] = 0;
    face_offsets[1] = 4;
    MutableSpan<int> corner_verts = source->corner_verts_for_write();
    corner_verts[0] = 0;
    corner_verts[1] = 1;
    corner_verts[2] = 2;
    corner_verts[3] = 3;
    bke::SpanAttributeWriter<float2> uvs = source->attributes_for_write()
                                               .lookup_or_add_for_write_span<float2>(
                                                   "UVMap", bke::AttrDomain::Corner);
    uvs.span[0] = float2(uv_min, uv_min);
    uvs.span[1] = float2(uv_max, uv_min);
    uvs.span[2] = float2(uv_max, uv_max);
    uvs.span[3] = float2(uv_min, uv_max);
    uvs.finish();
    source->tag_topology_changed();
    source->tag_positions_changed();
    BKE_mesh_nomain_to_mesh(source, surface_mesh, surface_object);
    mesh_ensure_default_uv_map(*surface_mesh);

    surface_mesh->texspace_flag = 0;
    surface_mesh->texspace_location[0] = 1.0f;
    surface_mesh->texspace_location[1] = 1.0f;
    surface_mesh->texspace_location[2] = 0.0f;
    surface_mesh->texspace_size[0] = 1.0f;
    surface_mesh->texspace_size[1] = 1.0f;
    surface_mesh->texspace_size[2] = 0.0f;
  }

  /** The evaluated object context a 3D Gradient bake reads; the test builds both by hand. */
  CorrectionsBakeSurface bake_surface() const
  {
    return {surface_object, surface_mesh};
  }

  /** Fill every texel of \a image with the opaque grey \a gray. */
  static void fill_image_gray(Image &image, const uchar gray)
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    ASSERT_NE(pixels, nullptr);
    for (const int64_t i : IndexRange(int64_t(ibuf->x) * ibuf->y)) {
      pixels[i * 4 + 0] = gray;
      pixels[i * 4 + 1] = gray;
      pixels[i * 4 + 2] = gray;
      pixels[i * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
  }

  static void image_size(Image &image, int &r_width, int &r_height)
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    r_width = ibuf->x;
    r_height = ibuf->y;
    BKE_image_release_ibuf(&image, ibuf, lock);
  }

  /** The texel of \a image's byte buffer, decoded to scene linear as the composite reads it. */
  static void map_pixel(Image &image, const int x, const int y, float r_rgba[4])
  {
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, &iuser, &lock);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->byte_data(), nullptr);
    const uchar *p = ibuf->byte_data() + (int64_t(y) * ibuf->x + x) * 4;
    float pixel[4] = {float(p[0]) / 255.0f,
                      float(p[1]) / 255.0f,
                      float(p[2]) / 255.0f,
                      float(p[3]) / 255.0f};
    if (ibuf->byte_buffer.colorspace != nullptr) {
      IMB_colormanagement_colorspace_to_scene_linear_v4(
          pixel, false, ibuf->byte_buffer.colorspace);
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
    copy_v4_v4(r_rgba, pixel);
  }

  /** The float texel of \a image's buffer, the format a Height map is baked and painted in. */
  static void map_pixel_float(Image &image, const int x, const int y, float r_rgba[4])
  {
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, &iuser, &lock);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->float_data(), nullptr);
    const int channels = ibuf->channels == 0 ? 4 : ibuf->channels;
    const float *p = ibuf->float_data() + (int64_t(y) * ibuf->x + x) * channels;
    float pixel[4] = {p[0], channels > 1 ? p[1] : p[0], channels > 2 ? p[2] : p[0],
                      channels > 3 ? p[3] : 1.0f};
    BKE_image_release_ibuf(&image, ibuf, lock);
    copy_v4_v4(r_rgba, pixel);
  }

  /**
   * What \a corrections lay together over nothing at the canvas \a size: the pre-merge reference
   * the merged map is compared against. \a surface is the context a 3D Gradient reads.
   */
  bool eval_corrections(const Vector<MaterialPaintLayer *> &corrections,
                        const eMaterialPaintChannel channel,
                        const int size,
                        const std::shared_ptr<const PaintMaterialCompositeSurface> &surface,
                        MutableSpan<float> r_color,
                        MutableSpan<float> r_coverage)
  {
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    iuser.tile = 1001;
    PaintMaterialCompositeImageLayer row;
    if (!BKE_paint_layers_corrections_row_describe(
            *ma, corrections, int(channel), size, &iuser, 1001, row, surface))
    {
      return false;
    }
    rcti area;
    BLI_rcti_init(&area, 0, size, 0, size);
    return BKE_paint_material_composite_eval_row_content(
        row, r_color.data(), r_coverage.data(), &area, int(channel));
  }

  /** The one correction the bake left on \a owner: the merged one, found by shape, not by name. */
  static MaterialPaintLayer *merged_correction(const MaterialPaintLayer &owner)
  {
    return static_cast<MaterialPaintLayer *>(owner.effects.first);
  }

  /** The map the merged correction holds in \a channel, or null when it holds none. */
  static Image *merged_map(const MaterialPaintLayer &merged, const eMaterialPaintChannel channel)
  {
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(merged, int(channel));
    return (record != nullptr) ? record->image : nullptr;
  }
};

TEST_F(PaintLayersCorrectionBakeTest, paint_fill_and_uv_gradient_merge_matches_composite)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));

  Image *paint_map = add_byte_image("PaintMap", 8);
  ASSERT_NE(paint_map, nullptr);
  fill_image_gray(*paint_map, 96);
  MaterialPaintLayer *paint = add_paint_correction(*row, "Paint", paint_map);
  const float start[3] = {0.0f, 0.5f, 0.0f};
  const float end[3] = {1.0f, 0.5f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_UV, start, end);
  const float fill_color[4] = {0.1f, 0.2f, 0.9f, 1.0f};
  MaterialPaintLayer *fill = add_fill_correction(*row, "Fill", fill_color, 0.5f);

  const Vector<MaterialPaintLayer *> run = {paint, gradient, fill};
  EXPECT_FALSE(BKE_paint_layers_corrections_surface_needed(*ma, run));
  /* What the three lay together over nothing, before anything is merged. */
  Vector<float> color(8 * 8 * 4);
  Vector<float> coverage(8 * 8);
  ASSERT_TRUE(eval_corrections(
      run, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 8, nullptr, color, coverage));

  MaterialPaintLayer *merged = BKE_paint_layers_corrections_bake(*bmain, *ma, run, nullptr);
  ASSERT_NE(merged, nullptr);
  ASSERT_EQ(BLI_listbase_count(&row->effects), 1);
  EXPECT_EQ(merged, merged_correction(*row));

  Image *map = merged_map(*merged, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(map, nullptr);
  for (const int2 texel : {int2(1, 1), int2(4, 7), int2(7, 3)}) {
    float pixel[4];
    map_pixel(*map, texel.x, texel.y, pixel);
    const int64_t i = int64_t(texel.y) * 8 + texel.x;
    EXPECT_NEAR(pixel[0], color[i * 4 + 0], 2.0f / 255.0f);
    EXPECT_NEAR(pixel[1], color[i * 4 + 1], 2.0f / 255.0f);
    EXPECT_NEAR(pixel[2], color[i * 4 + 2], 2.0f / 255.0f);
    /* The map's alpha is the coverage the corrections brought. */
    EXPECT_NEAR(pixel[3], coverage[i], 2.0f / 255.0f);
  }
}

TEST_F(PaintLayersCorrectionBakeTest, gradient_canvas_follows_the_maps)
{
  /* The material pins no size, so the Paint correction's 16x16 map decides the canvas. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  Image *paint_map = add_byte_image("PaintMap", 16);
  ASSERT_NE(paint_map, nullptr);
  fill_image_gray(*paint_map, 0);
  MaterialPaintLayer *paint = add_paint_correction(*row, "Paint", paint_map);
  const float start[3] = {0.0f, 0.5f, 0.0f};
  const float end[3] = {1.0f, 0.5f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_UV, start, end);

  const Vector<MaterialPaintLayer *> run = {paint, gradient};
  MaterialPaintLayer *merged = BKE_paint_layers_corrections_bake(*bmain, *ma, run, nullptr);
  ASSERT_NE(merged, nullptr);

  Image *map = merged_map(*merged, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(map, nullptr);
  int width = 0;
  int height = 0;
  image_size(*map, width, height);
  EXPECT_EQ(width, 16);
  EXPECT_EQ(height, 16);
  /* The UV line spans the unit square of the map: texel x is grey (x + 0.5) / 16. A grid left at
   * the description's own size would read a fraction of the ramp instead. */
  for (const int x : {2, 8, 13}) {
    float pixel[4];
    map_pixel(*map, x, 5, pixel);
    const float expected = (float(x) + 0.5f) / 16.0f;
    EXPECT_NEAR(pixel[0], expected, 2.0f / 255.0f);
    EXPECT_NEAR(pixel[1], expected, 2.0f / 255.0f);
    EXPECT_NEAR(pixel[2], expected, 2.0f / 255.0f);
    EXPECT_NEAR(pixel[3], 1.0f, 2.0f / 255.0f);
  }
}

TEST_F(PaintLayersCorrectionBakeTest, object_gradient_merge_uses_surface)
{
  create_surface_quad();
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  /* Object X of 0..2 at Y = 1: the texel's sampled position (2u, 2v, 0) reads t = u. */
  const float start[3] = {0.0f, 1.0f, 0.0f};
  const float end[3] = {2.0f, 1.0f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT, start, end);
  const Vector<MaterialPaintLayer *> run = {gradient};
  EXPECT_TRUE(BKE_paint_layers_corrections_surface_needed(*ma, run));

  /* The reference: the same surface the bake builds, at the same canvas. */
  std::shared_ptr<const PaintMaterialCompositeSurface> surface =
      BKE_paint_layers_gradient_surface_build(*surface_object,
                                              *surface_mesh,
                                              BKE_paint_layers_gradient_uv_map_name(
                                                  *surface_mesh)
                                                  .c_str(),
                                              8);
  ASSERT_NE(surface, nullptr);
  Vector<float> color(8 * 8 * 4);
  Vector<float> coverage(8 * 8);
  ASSERT_TRUE(
      eval_corrections(run, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 8, surface, color, coverage));

  MaterialPaintLayer *merged = BKE_paint_layers_corrections_bake(
      *bmain, *ma, run, nullptr, bake_surface());
  ASSERT_NE(merged, nullptr);
  ASSERT_EQ(BLI_listbase_count(&row->effects), 1);

  Image *map = merged_map(*merged, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(map, nullptr);
  for (const int x : {1, 4, 6}) {
    float pixel[4];
    map_pixel(*map, x, 4, pixel);
    /* The absolute expectation: the line reads object X of 0..2, so texel x is grey
     * (x + 0.5) / 8. */
    const float expected = (float(x) + 0.5f) / 8.0f;
    EXPECT_NEAR(pixel[0], expected, 3.0f / 255.0f);
    EXPECT_NEAR(pixel[1], expected, 3.0f / 255.0f);
    EXPECT_NEAR(pixel[2], expected, 3.0f / 255.0f);
    EXPECT_NEAR(pixel[3], 1.0f, 2.0f / 255.0f);
  }
  /* And the wiring: the map holds what the pre-merge evaluation laid through the same surface. */
  for (const int2 texel : {int2(0, 0), int2(5, 2), int2(7, 7)}) {
    float pixel[4];
    map_pixel(*map, texel.x, texel.y, pixel);
    const int64_t i = int64_t(texel.y) * 8 + texel.x;
    EXPECT_NEAR(pixel[0], color[i * 4 + 0], 2.0f / 255.0f);
    EXPECT_NEAR(pixel[1], color[i * 4 + 1], 2.0f / 255.0f);
    EXPECT_NEAR(pixel[2], color[i * 4 + 2], 2.0f / 255.0f);
    EXPECT_NEAR(pixel[3], coverage[i], 2.0f / 255.0f);
  }
}

TEST_F(PaintLayersCorrectionBakeTest, generated_gradient_merge_uses_surface)
{
  create_surface_quad();
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  /* Generated X of 0..1 at Y = 0.5: with the texspace box the quad's generated coordinate is
   * (u, v, 0.5), so the line reads t = u. */
  const float start[3] = {0.0f, 0.5f, 0.0f};
  const float end[3] = {1.0f, 0.5f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED, start, end);
  const Vector<MaterialPaintLayer *> run = {gradient};
  EXPECT_TRUE(BKE_paint_layers_corrections_surface_needed(*ma, run));

  std::shared_ptr<const PaintMaterialCompositeSurface> surface =
      BKE_paint_layers_gradient_surface_build(*surface_object,
                                              *surface_mesh,
                                              BKE_paint_layers_gradient_uv_map_name(
                                                  *surface_mesh)
                                                  .c_str(),
                                              8);
  ASSERT_NE(surface, nullptr);
  Vector<float> color(8 * 8 * 4);
  Vector<float> coverage(8 * 8);
  ASSERT_TRUE(
      eval_corrections(run, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 8, surface, color, coverage));

  MaterialPaintLayer *merged = BKE_paint_layers_corrections_bake(
      *bmain, *ma, run, nullptr, bake_surface());
  ASSERT_NE(merged, nullptr);

  Image *map = merged_map(*merged, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(map, nullptr);
  for (const int x : {1, 4, 6}) {
    float pixel[4];
    map_pixel(*map, x, 4, pixel);
    const float expected = (float(x) + 0.5f) / 8.0f;
    EXPECT_NEAR(pixel[0], expected, 3.0f / 255.0f);
    EXPECT_NEAR(pixel[1], expected, 3.0f / 255.0f);
    EXPECT_NEAR(pixel[2], expected, 3.0f / 255.0f);
    EXPECT_NEAR(pixel[3], 1.0f, 2.0f / 255.0f);
  }
}

TEST_F(PaintLayersCorrectionBakeTest, object_gradient_without_a_surface_context_is_refused)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  const float start[3] = {0.0f, 1.0f, 0.0f};
  const float end[3] = {2.0f, 1.0f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT, start, end);
  const Vector<MaterialPaintLayer *> run = {gradient};

  /* The shape is a valid single-correction conversion; what is missing is the object. */
  EXPECT_EQ(BKE_paint_layers_corrections_bake_kind(*ma, run), 1);
  EXPECT_TRUE(BKE_paint_layers_corrections_surface_needed(*ma, run));
  /* A disabled correction lays nothing, so it asks for no surface either. */
  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, gradient, false));
  EXPECT_FALSE(BKE_paint_layers_corrections_surface_needed(*ma, run));
  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, gradient, true));

  EXPECT_FALSE(BKE_paint_layers_corrections_bake_precheck(*ma, run, nullptr));
  EXPECT_EQ(BKE_paint_layers_corrections_bake(*bmain, *ma, run, nullptr), nullptr);
  /* Nothing was merged and nothing was removed. */
  EXPECT_EQ(BLI_listbase_count(&row->effects), 1);
  EXPECT_EQ(row->effects.first, gradient);
}

TEST_F(PaintLayersCorrectionBakeTest, object_gradient_on_udim_tiles_beyond_1001_is_refused)
{
  create_surface_quad();
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));

  /* A tiled Paint correction map holding tile 1002 beside 1001. */
  Image *tiled = add_byte_image("TiledPaint", 8, true);
  ASSERT_NE(tiled, nullptr);
  fill_image_gray(*tiled, 96);
  ASSERT_TRUE(BKE_paint_layers_map_coverage_tile_ensure(*tiled, 1002));
  MaterialPaintLayer *paint = add_paint_correction(*row, "Paint", tiled);
  const float start[3] = {0.0f, 1.0f, 0.0f};
  const float end[3] = {2.0f, 1.0f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT, start, end);
  const Vector<MaterialPaintLayer *> run = {paint, gradient};

  /* The surface context is fine, so the precheck passes; the tiles are the bake's to refuse. */
  EXPECT_TRUE(BKE_paint_layers_corrections_bake_precheck(*ma, run, nullptr, bake_surface()));
  EXPECT_EQ(
      BKE_paint_layers_corrections_bake(*bmain, *ma, run, nullptr, bake_surface()), nullptr);
  EXPECT_EQ(BLI_listbase_count(&row->effects), 2);
}

TEST_F(PaintLayersCorrectionBakeTest, a_disabled_gradient_asks_for_no_surface)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  const float start[3] = {0.0f, 1.0f, 0.0f};
  const float end[3] = {2.0f, 1.0f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT, start, end);
  const Vector<MaterialPaintLayer *> run = {gradient};
  EXPECT_TRUE(BKE_paint_layers_corrections_surface_needed(*ma, run));

  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, gradient, false));
  EXPECT_FALSE(BKE_paint_layers_corrections_surface_needed(*ma, run));
}

TEST_F(PaintLayersCorrectionBakeTest, a_refused_surface_bake_creates_no_maps_and_keeps_the_rows)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  const float start[3] = {0.0f, 1.0f, 0.0f};
  const float end[3] = {2.0f, 1.0f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT, start, end);
  const Vector<MaterialPaintLayer *> run = {gradient};
  const int images_before = bmain->images.count();
  const int rows_before = BLI_listbase_count(&row->effects);

  EXPECT_FALSE(BKE_paint_layers_corrections_bake_precheck(*ma, run, nullptr));
  EXPECT_EQ(BKE_paint_layers_corrections_bake(*bmain, *ma, run, nullptr), nullptr);
  /* A refusal leaves the description as it was: no map, no merged correction. */
  EXPECT_EQ(bmain->images.count(), images_before);
  EXPECT_EQ(BLI_listbase_count(&row->effects), rows_before);
}

TEST_F(PaintLayersCorrectionBakeTest, a_material_and_a_gradient_without_a_surface_is_refused)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  Material *source = BKE_material_add(bmain, "BakeSource");
  ASSERT_NE(source, nullptr);
  MaterialPaintLayer *material_correction = BKE_paint_layers_correction_add_material(
      *ma, row, "Material", source);
  ASSERT_NE(material_correction, nullptr);
  const float start[3] = {0.0f, 1.0f, 0.0f};
  const float end[3] = {2.0f, 1.0f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT, start, end);
  const Vector<MaterialPaintLayer *> run = {material_correction, gradient};

  /* The Material escape in the precheck must not hide the surface the Gradient needs. */
  EXPECT_FALSE(BKE_paint_layers_corrections_bake_precheck(*ma, run, nullptr));
  EXPECT_EQ(BKE_paint_layers_corrections_bake(*bmain, *ma, run, nullptr), nullptr);
  EXPECT_EQ(BLI_listbase_count(&row->effects), 2);
}

TEST_F(PaintLayersCorrectionBakeTest, a_mesh_without_a_uv_map_refuses_the_bake)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  Object *bare_object = BKE_object_add_only_object(bmain, OB_MESH, "NoUv");
  Mesh *bare_mesh = BKE_mesh_add(bmain, "NoUvMesh");
  bare_object->data = &bare_mesh->id;
  const float start[3] = {0.0f, 1.0f, 0.0f};
  const float end[3] = {2.0f, 1.0f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT, start, end);
  const Vector<MaterialPaintLayer *> run = {gradient};
  const CorrectionsBakeSurface surface{bare_object, bare_mesh};

  EXPECT_TRUE(BKE_paint_layers_gradient_uv_map_name(*bare_mesh).is_empty());
  EXPECT_FALSE(BKE_paint_layers_corrections_bake_precheck(*ma, run, nullptr, surface));
  EXPECT_EQ(BKE_paint_layers_corrections_bake(*bmain, *ma, run, nullptr, surface), nullptr);
  /* And the surface itself cannot be rasterized without a UV map. */
  EXPECT_FALSE(BKE_paint_layers_gradient_surface_build(*bare_object, *bare_mesh, "", 8));
  EXPECT_EQ(BLI_listbase_count(&row->effects), 1);
}

TEST_F(PaintLayersCorrectionBakeTest, an_object_gradient_pads_the_island_border)
{
  /* The surface never resolves below 256, so the canvas matches that to make one surface texel one
   * map texel. The island covers UV 0.25..0.75 (texels 64..191), and 256 / 64 gives the four
   * texels of padding that land on 60..195. */
  create_surface_quad(0.25f, 0.75f);
  ma->paint_layers_image_size = 256;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  const float start[3] = {0.0f, 1.0f, 0.0f};
  const float end[3] = {2.0f, 1.0f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT, start, end);
  const Vector<MaterialPaintLayer *> run = {gradient};

  MaterialPaintLayer *merged = BKE_paint_layers_corrections_bake(
      *bmain, *ma, run, nullptr, bake_surface());
  ASSERT_NE(merged, nullptr);
  Image *map = merged_map(*merged, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(map, nullptr);

  float pixel[4];
  /* Just inside the padding: bilinear filtering of the map reads the island edge there. */
  map_pixel(*map, 60, 128, pixel);
  EXPECT_GT(pixel[3], 0.5f);
  map_pixel(*map, 195, 128, pixel);
  EXPECT_GT(pixel[3], 0.5f);
  /* One texel past the padding nothing was laid, and the texel keeps the transparent fill. */
  map_pixel(*map, 59, 128, pixel);
  EXPECT_NEAR(pixel[3], 0.0f, 1.0f / 255.0f);
  map_pixel(*map, 196, 128, pixel);
  EXPECT_NEAR(pixel[3], 0.0f, 1.0f / 255.0f);
}

TEST_F(PaintLayersCorrectionBakeTest, a_gradient_and_a_fill_bake_as_a_merge)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  const float start[3] = {0.0f, 0.5f, 0.0f};
  const float end[3] = {1.0f, 0.5f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_UV, start, end);
  const float color[4] = {0.2f, 0.4f, 0.8f, 1.0f};
  MaterialPaintLayer *fill = add_fill_correction(*row, "Fill", color, 0.5f);
  const Vector<MaterialPaintLayer *> run = {gradient, fill};

  EXPECT_EQ(BKE_paint_layers_corrections_bake_kind(*ma, run), 2);
}

TEST_F(PaintLayersCorrectionBakeTest, a_single_gradient_converts_to_a_texture)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  const float start[3] = {0.0f, 0.5f, 0.0f};
  const float end[3] = {1.0f, 0.5f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_UV, start, end);
  const Vector<MaterialPaintLayer *> run = {gradient};

  EXPECT_EQ(BKE_paint_layers_corrections_bake_kind(*ma, run), 1);
  MaterialPaintLayer *merged = BKE_paint_layers_corrections_bake(*bmain, *ma, run, nullptr);
  ASSERT_NE(merged, nullptr);
  EXPECT_EQ(merged->source, MA_PAINT_LAYER_SOURCE_IMAGE);

  Image *map = merged_map(*merged, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(map, nullptr);
  /* The default black-to-white line across the unit square: grey (x + 0.5) / 8 at texel x, laid
   * everywhere at full coverage. */
  for (const int x : {0, 3, 7}) {
    float pixel[4];
    map_pixel(*map, x, 4, pixel);
    const float expected = (float(x) + 0.5f) / 8.0f;
    EXPECT_NEAR(pixel[0], expected, 2.0f / 255.0f);
    EXPECT_NEAR(pixel[1], expected, 2.0f / 255.0f);
    EXPECT_NEAR(pixel[2], expected, 2.0f / 255.0f);
    EXPECT_NEAR(pixel[3], 1.0f, 2.0f / 255.0f);
  }
}

TEST_F(PaintLayersCorrectionBakeTest, a_scalar_channel_gradient_merges_its_value_ends)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  const float start[3] = {0.0f, 0.5f, 0.0f};
  const float end[3] = {1.0f, 0.5f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_UV, start, end);
  /* Roughness ramps between the record's own ends: grey lerp(0.25, 0.75) along the line. */
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, gradient, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
            nullptr);
  EXPECT_TRUE(BKE_paint_layers_gradient_set_channel_scalars(
      *ma, *gradient, PAINT_MATERIAL_CHANNEL_ROUGHNESS, 0.25f, 0.75f));

  const Vector<MaterialPaintLayer *> run = {gradient};
  MaterialPaintLayer *merged = BKE_paint_layers_corrections_bake(*bmain, *ma, run, nullptr);
  ASSERT_NE(merged, nullptr);

  Image *map = merged_map(*merged, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(map, nullptr);
  for (const int x : {1, 4, 7}) {
    float pixel[4];
    map_pixel(*map, x, 2, pixel);
    const float t = (float(x) + 0.5f) / 8.0f;
    const float expected = 0.25f + (0.75f - 0.25f) * t;
    EXPECT_NEAR(pixel[0], expected, 2.0f / 255.0f);
    EXPECT_NEAR(pixel[1], expected, 2.0f / 255.0f);
    EXPECT_NEAR(pixel[2], expected, 2.0f / 255.0f);
    EXPECT_NEAR(pixel[3], 1.0f, 2.0f / 255.0f);
  }
}

TEST_F(PaintLayersCorrectionBakeTest, a_height_gradient_correction_stays_in_the_merge)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));
  const float start[3] = {0.0f, 0.5f, 0.0f};
  const float end[3] = {1.0f, 0.5f, 0.0f};
  MaterialPaintLayer *gradient = add_gradient_correction(
      *row, "Gradient", MA_PAINT_LAYER_GRADIENT_SPACE_UV, start, end);
  /* Height has to be in the material's set and laid by the correction, or the merge would drop the
   * Height write silently (the bug the bake of a layer-paint channel closes). */
  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_HEIGHT, true));
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, gradient, PAINT_MATERIAL_CHANNEL_HEIGHT), nullptr);
  /* Relief ends -0.5..0.5, stored as 0.25..0.75. */
  ASSERT_TRUE(BKE_paint_layers_gradient_set_channel_scalars(
      *ma, *gradient, PAINT_MATERIAL_CHANNEL_HEIGHT, -0.5f, 0.5f));

  const Vector<MaterialPaintLayer *> run = {gradient};
  MaterialPaintLayer *merged = BKE_paint_layers_corrections_bake(*bmain, *ma, run, nullptr);
  ASSERT_NE(merged, nullptr);

  Image *map = merged_map(*merged, PAINT_MATERIAL_CHANNEL_HEIGHT);
  ASSERT_NE(map, nullptr);
  /* Height is baked as a float map - the continuous field, not the byte factor every other scalar
   * channel gets - and ramps between the correction's own ends along the line. */
  for (const int x : {1, 4, 7}) {
    float pixel[4];
    map_pixel_float(*map, x, 2, pixel);
    const float t = (float(x) + 0.5f) / 8.0f;
    const float expected = 0.25f + (0.75f - 0.25f) * t;
    EXPECT_NEAR(pixel[0], expected, 1e-4f);
    EXPECT_NEAR(pixel[1], expected, 1e-4f);
    EXPECT_NEAR(pixel[2], expected, 1e-4f);
    EXPECT_GT(pixel[3], 0.5f);
  }
}

TEST_F(PaintLayersCorrectionBakeTest, filters_and_anchor_points_are_still_refused)
{
  ma->paint_layers_image_size = 8;
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("RowMap"));

  MaterialPaintLayer *filter = add_paint_correction(*row, "Filter", add_solid_image("FilterMap"));
  filter->source = MA_PAINT_LAYER_SOURCE_FILTER;
  filter->kind_subtype = MA_PAINT_LAYER_FILTER_INVERT;
  Vector<MaterialPaintLayer *> run = {filter};
  EXPECT_EQ(BKE_paint_layers_corrections_bake_kind(*ma, run), 0);
  EXPECT_EQ(BKE_paint_layers_corrections_bake(*bmain, *ma, run, nullptr), nullptr);
  EXPECT_EQ(BLI_listbase_count(&row->effects), 1);

  MaterialPaintLayer *marker = add_paint_correction(*row, "Marker", add_solid_image("MarkerMap"));
  marker->source = MA_PAINT_LAYER_SOURCE_ANCHOR_POINT;
  marker->kind_subtype = MA_PAINT_LAYER_ANCHOR_POINT_MARKER;
  run = {marker};
  EXPECT_EQ(BKE_paint_layers_corrections_bake(*bmain, *ma, run, nullptr), nullptr);
  EXPECT_EQ(BLI_listbase_count(&row->effects), 2);
}

}  // namespace blender::bke::tests
