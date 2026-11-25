/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The Smart Filter asset type and the chain instance it lays: what the save refuses (the members
 * must be pointwise filter corrections the composite unfolding can lay, or GPU-computed kinds it
 * lays as effect corrections keyed on composite markers), what the insert lays (one chain row
 * reading the asset material, its exposed values seeded under the member keys the dynamic
 * descriptor names), and what the description unfolds (the same corrections the members describe
 * by hand, the instance's overrides included -- and a GPU-computed member whose result is the
 * member's kernel over the chain's content, matching the stack the user would have built by
 * hand).
 */

#include "testing/testing.h"

#include <cmath>
#include <string>

#include "BKE_blender.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_asset.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_tile.hh"
#include "BKE_paint_material_composite.hh"

#include "IMB_imbuf.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "DNA_material_types.h"

#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

namespace blender::bke::tests {

class PaintLayersChainTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  /** Scratch for the composite descriptions the parity test reads. */
  Vector<PaintMaterialCompositeImageLayer> layers_;

  void SetUp() override
  {
    paint_layer_anchor_point_slice_cache_clear();
    BKE_paint_layers_effect_cache_free_all();
    bmain = BKE_main_new();
    BKE_blender_globals_main_replace(bmain);
  }

  void TearDown() override
  {
    paint_layer_anchor_point_slice_cache_clear();
    BKE_paint_layers_effect_cache_free_all();
    G_MAIN = nullptr;
    BKE_main_free(bmain);
  }

  /** A flat Fill row of the stack, the shape the parity test describes with no map at all. */
  MaterialPaintLayer *add_fill_row(Material &ma, const char *name, const float value[4])
  {
    MaterialPaintLayer *row = BKE_paint_layers_add(ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(row, nullptr);
    EXPECT_NE(BKE_paint_layers_channel_add(ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_value(
        ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, value));
    return row;
  }

  /** An opaque 8x8 byte image whose grey runs from black at the left edge to white at the right
   * one: the map a row paints, so a blur member's neighbourhood reads an edge and not a flat
   * field, and the parity test compares a real answer and not a kernel's identity. */
  Image *add_ramp_image(const char *name)
  {
    const int size = 8;
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(size)) {
      for (const int x : IndexRange(size)) {
        const uchar value = uchar(x * 255 / (size - 1));
        uchar *p = pixels + (int64_t(y) * size + x) * 4;
        p[0] = value;
        p[1] = value;
        p[2] = value;
        p[3] = 255;
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  /** A Paint row whose Base Color is the painted \a image: the shape a GPU-computed member's
   * cache answer is rendered over. */
  MaterialPaintLayer *add_map_row(Material &ma, const char *name, Image *image)
  {
    MaterialPaintLayer *row = BKE_paint_layers_add(ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(row, nullptr);
    EXPECT_NE(BKE_paint_layers_channel_add(ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
    EXPECT_TRUE(
        BKE_paint_layers_channel_set_image(ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image));
    return row;
  }

  /** The composite of Base Color as straight scene-linear RGBA at the stack's dimensions. */
  bool render_base_color(Material &ma, Vector<float> &r_pixels, int &r_width, int &r_height)
  {
    Vector<PaintMaterialCompositeImageLayer> layers;
    if (!BKE_paint_layers_composite_image_layers(
            ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers))
    {
      return false;
    }
    if (!BKE_paint_material_composite_stack_dimensions(layers, r_width, r_height) ||
        r_width <= 0 || r_height <= 0)
    {
      return false;
    }
    r_pixels.resize(int64_t(r_width) * r_height * 4);
    float bottom[4];
    BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_BASE_COLOR, bottom);
    return BKE_paint_material_composite_eval_images_linear(
        layers,
        PaintMaterialCompositeLinearTarget::frame(r_pixels, r_width),
        nullptr,
        nullptr,
        bottom);
  }
};

/** The first effect of \a row, or null. */
static MaterialPaintLayer *first_effect(MaterialPaintLayer &row)
{
  return static_cast<MaterialPaintLayer *>(row.effects.first);
}

/** The chain instance the insert laid: the one effect of \a row, or null. */
static MaterialPaintLayer *chain_effect_of(MaterialPaintLayer &row)
{
  MaterialPaintLayer *effect = first_effect(row);
  if (effect == nullptr || effect->kind_subtype != MA_PAINT_LAYER_FILTER_CHAIN) {
    return nullptr;
  }
  return effect;
}

/** The definition's first effect row of \a kind, or null. */
static MaterialPaintLayer *definition_member_of_kind(MaterialPaintLayer &chain,
                                                     const eMaterialPaintLayerFilter kind)
{
  Material *definition = chain.material;
  if (definition == nullptr) {
    return nullptr;
  }
  for (MaterialPaintLayer &member : definition->paint_layers) {
    if (member.kind_subtype == kind) {
      return &member;
    }
  }
  return nullptr;
}

/**
 * The pointwise members a conforming save holds, laid on one row: an Exposure with a moved
 * exposure value and a Contrast Luminosity with a moved contrast. The pair exercises both schema
 * orders -- one parameter and two -- and both stay pointwise whatever their values are.
 */
struct ChainMembers {
  MaterialPaintLayer *exposure = nullptr;
  MaterialPaintLayer *contrast = nullptr;
};

static ChainMembers add_pointwise_members(Material &ma, MaterialPaintLayer &row)
{
  ChainMembers members;
  members.exposure = BKE_paint_layers_filter_add(
      ma, &row, MA_PAINT_LAYER_FILTER_EXPOSURE, "Exposure");
  EXPECT_NE(members.exposure, nullptr);
  members.contrast = BKE_paint_layers_filter_add(
      ma, &row, MA_PAINT_LAYER_FILTER_CONTRAST_LUMINOSITY, "Contrast");
  EXPECT_NE(members.contrast, nullptr);
  /* The values a conforming save carries and the parity test compares: both away from the
   * schema's defaults, so a seeded instance is distinguishable from an unseeded one. */
  EXPECT_TRUE(paint_layer_kind_param_set(
      *members.exposure, PAINT_LAYER_EXPOSURE_EXPOSURE, 1.5f));
  EXPECT_TRUE(paint_layer_kind_param_set(
      *members.contrast, PAINT_LAYER_CONTRAST_LUMINOSITY_CONTRAST, 0.75f));
  return members;
}

TEST_F(PaintLayersChainTest, smart_filter_portability_refuses_non_pointwise_members)
{
  Material *src = BKE_material_add(bmain, "Src");
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  MaterialPaintLayer *row = add_fill_row(*src, "Row", white);

  /* A Blur reads a neighbourhood, but the preview lays an effect pass for it: the contract lets a
   * GPU-computed member through, and the unfolding lays it as an effect correction keyed on a
   * composite marker rather than as the pointwise filter it is not. */
  MaterialPaintLayer *blur = BKE_paint_layers_filter_add(*src, row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(blur, nullptr);
  const MaterialPaintLayer *blur_rows[1] = {blur};
  const PaintLayerAssetPortability blur_check = BKE_paint_layers_asset_portability_check(
      *src, Span<const MaterialPaintLayer *>(blur_rows, 1), PaintLayerAssetType::Filter);
  EXPECT_EQ(blur_check.chain_member_num, 1);
  EXPECT_EQ(blur_check.chain_unsupported_num, 0);
  EXPECT_FALSE(blur_check.blocking());

  /* A Normal to Mask reads an Anchor Point's slice -- and it is the NEEDS_BAKE-without-GPU_COMPUTED
   * shape the contract keeps out: the per-member effect path would work for it on the CPU, but the
   * GPU refusal matrix would reject whole stacks for it, and a member is either pointwise or has a
   * per-member GPU answer (a GPU pass, or the node-group filter's precomputed one). */
  MaterialPaintLayer *normal_mask = BKE_paint_layers_filter_add(
      *src, row, MA_PAINT_LAYER_FILTER_NORMAL_TO_MASK);
  ASSERT_NE(normal_mask, nullptr);
  const MaterialPaintLayer *mask_rows[1] = {normal_mask};
  const PaintLayerAssetPortability mask_check = BKE_paint_layers_asset_portability_check(
      *src, Span<const MaterialPaintLayer *>(mask_rows, 1), PaintLayerAssetType::Filter);
  EXPECT_EQ(mask_check.chain_member_num, 0);
  EXPECT_EQ(mask_check.chain_unsupported_num, 1);
  EXPECT_TRUE(mask_check.blocking());

  /* A stack row is not a member at all: the definition holds effect rows only. */
  const MaterialPaintLayer *row_rows[1] = {row};
  const PaintLayerAssetPortability row_check = BKE_paint_layers_asset_portability_check(
      *src, Span<const MaterialPaintLayer *>(row_rows, 1), PaintLayerAssetType::Filter);
  EXPECT_EQ(row_check.chain_unsupported_num, 1);
  EXPECT_TRUE(row_check.blocking());

  /* Neither type blocks a Smart Material save: the counts belong to the Filter walk alone. */
  const MaterialPaintLayer *both_rows[2] = {row, blur};
  const PaintLayerAssetPortability material_check = BKE_paint_layers_asset_portability_check(
      *src, Span<const MaterialPaintLayer *>(both_rows, 2), PaintLayerAssetType::Material);
  EXPECT_EQ(material_check.chain_unsupported_num, 0);
  EXPECT_FALSE(material_check.blocking());

  /* A chain row itself is not a member: this build unfolds one level, and a chain of chains is a
   * composite plan of its own. The insert lays one, so the shape is reachable. */
  Material *host = BKE_material_add(bmain, "ChainHost");
  MaterialPaintLayer *host_row = add_fill_row(*host, "HostRow", white);
  MaterialPaintLayer *exposure = BKE_paint_layers_filter_add(
      *host, host_row, MA_PAINT_LAYER_FILTER_EXPOSURE, "Exposure");
  ASSERT_NE(exposure, nullptr);
  const MaterialPaintLayer *pointwise_rows[1] = {exposure};
  Material *pointwise_asset = BKE_paint_layers_asset_create(
      *bmain,
      *host,
      Span<const MaterialPaintLayer *>(pointwise_rows, 1),
      PaintLayerAssetType::Filter,
      "NestedSource");
  ASSERT_NE(pointwise_asset, nullptr);
  MaterialPaintLayer *chain = BKE_paint_layers_filter_add(
      *host, host_row, MA_PAINT_LAYER_FILTER_CHAIN, "Nested");
  ASSERT_NE(chain, nullptr);
  chain->material = pointwise_asset;
  id_us_plus(&pointwise_asset->id);
  const MaterialPaintLayer *chain_rows[1] = {chain};
  const PaintLayerAssetPortability chain_check = BKE_paint_layers_asset_portability_check(
      *host, Span<const MaterialPaintLayer *>(chain_rows, 1), PaintLayerAssetType::Filter);
  EXPECT_EQ(chain_check.chain_member_num, 0);
  EXPECT_EQ(chain_check.chain_unsupported_num, 1);
  EXPECT_TRUE(chain_check.blocking());

  /* The create refuses what the walk blocks, without leaking the asset material. */
  const int materials_before = BLI_listbase_count(&bmain->materials);
  EXPECT_EQ(BKE_paint_layers_asset_create(*bmain,
                                          *src,
                                          Span<const MaterialPaintLayer *>(mask_rows, 1),
                                          PaintLayerAssetType::Filter,
                                          "BlockedNormalMask"),
            nullptr);
  EXPECT_EQ(BLI_listbase_count(&bmain->materials), materials_before);

  /* A conforming pair passes: two members judged, nothing refused, and the create holds both. */
  MaterialPaintLayer *pointwise_row = add_fill_row(*src, "PointwiseRow", white);
  const ChainMembers members = add_pointwise_members(*src, *pointwise_row);
  const MaterialPaintLayer *member_rows[2] = {members.exposure, members.contrast};
  const PaintLayerAssetPortability ok_check = BKE_paint_layers_asset_portability_check(
      *src, Span<const MaterialPaintLayer *>(member_rows, 2), PaintLayerAssetType::Filter);
  EXPECT_EQ(ok_check.chain_member_num, 2);
  EXPECT_EQ(ok_check.chain_unsupported_num, 0);
  EXPECT_FALSE(ok_check.blocking());

  Material *asset = BKE_paint_layers_asset_create(
      *bmain, *src, Span<const MaterialPaintLayer *>(member_rows, 2), PaintLayerAssetType::Filter,
      "ConformingChain");
  ASSERT_NE(asset, nullptr);
  EXPECT_EQ(BKE_paint_layers_asset_type_get(*asset), PaintLayerAssetType::Filter);
  /* The definition is the members themselves: effect rows at the root, roles and kinds kept. */
  ASSERT_EQ(BLI_listbase_count(&asset->paint_layers), 2);
  MaterialPaintLayer *asset_exposure = static_cast<MaterialPaintLayer *>(
      asset->paint_layers.first);
  ASSERT_NE(asset_exposure, nullptr);
  EXPECT_EQ(asset_exposure->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(asset_exposure->source, MA_PAINT_LAYER_SOURCE_FILTER);
  EXPECT_EQ(asset_exposure->kind_subtype, MA_PAINT_LAYER_FILTER_EXPOSURE);
  MaterialPaintLayer *asset_contrast = asset_exposure->next;
  ASSERT_NE(asset_contrast, nullptr);
  EXPECT_EQ(asset_contrast->kind_subtype, MA_PAINT_LAYER_FILTER_CONTRAST_LUMINOSITY);
  /* The member's value came with the copy, and the exposed list names it. */
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(
                      *asset_exposure, PAINT_LAYER_EXPOSURE_EXPOSURE),
                  1.5f);
  ASSERT_TRUE(BKE_paint_layers_asset_params_has(*asset));
  const IDProperty *params = BKE_paint_layers_asset_params_get(*asset);
  ASSERT_NE(IDP_GetPropertyFromGroup(params, "Exposure"), nullptr);
  ASSERT_NE(IDP_GetPropertyFromGroup(params, "Contrast"), nullptr);
}

TEST_F(PaintLayersChainTest, smart_filter_insert_lays_one_chain_row_with_seeded_overrides)
{
  Material *src = BKE_material_add(bmain, "Src");
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  MaterialPaintLayer *row = add_fill_row(*src, "Row", white);
  const ChainMembers members = add_pointwise_members(*src, *row);
  const MaterialPaintLayer *member_rows[2] = {members.exposure, members.contrast};

  Material *asset = BKE_paint_layers_asset_create(
      *bmain, *src, Span<const MaterialPaintLayer *>(member_rows, 2), PaintLayerAssetType::Filter,
      "SeededChain");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *anchor = add_fill_row(*dst, "Target", white);

  const char *reason = nullptr;
  EXPECT_TRUE(BKE_paint_layers_asset_can_insert(
      *dst, *asset, anchor, PaintLayerPlace::Above, &reason));
  EXPECT_EQ(reason, nullptr);
  /* A chain lands on a row: no target, no landing, whatever the place says. */
  reason = nullptr;
  EXPECT_FALSE(BKE_paint_layers_asset_can_insert(
      *dst, *asset, nullptr, PaintLayerPlace::Above, &reason));
  EXPECT_NE(reason, nullptr);
  /* Not a layered material: nothing to land in. */
  Material *plain = BKE_material_add(bmain, "Plain");
  reason = nullptr;
  EXPECT_FALSE(BKE_paint_layers_asset_can_insert(
      *plain, *asset, nullptr, PaintLayerPlace::Above, &reason));
  EXPECT_NE(reason, nullptr);

  const int stack_before = BLI_listbase_count(&dst->paint_layers);
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_asset_insert(
      *bmain, *dst, *asset, anchor, PaintLayerPlace::Above, &result));
  /* The insert imports no rows: the stack keeps its shape, the chain rides the target. */
  EXPECT_EQ(BLI_listbase_count(&dst->paint_layers), stack_before);
  EXPECT_EQ(result.rows, 1);
  EXPECT_EQ(result.first, anchor);
  ASSERT_EQ(result.imported.size(), 1);

  MaterialPaintLayer *chain = chain_effect_of(*anchor);
  ASSERT_NE(chain, nullptr);
  EXPECT_EQ(chain->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(chain->source, MA_PAINT_LAYER_SOURCE_FILTER);
  EXPECT_STREQ(chain->name, asset->id.name + 2);
  /* The definition link is the asset material, as a user of it. */
  EXPECT_EQ(chain->material, asset);
  EXPECT_GT(asset->id.us, 0);

  /* The record the drop action writes names the chain row: the instance panel and Make Unique
   * find it the way they find any other drop's rows. */
  const int instance_id = BKE_paint_layers_asset_instance_record(
      *dst, *asset, PaintLayerAssetType::Filter, result.imported.as_span());
  ASSERT_EQ(instance_id, 1);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*dst, *chain), instance_id);
  EXPECT_EQ(BKE_paint_layers_asset_instance_label(*dst, instance_id), "SeededChain #1");

  /* The instance's values are filed under the member keys the dynamic descriptor names, and each
   * carries the member's stored value at save time. The same key the descriptor built is what the
   * composite reads back, so the two cannot drift. */
  MaterialPaintLayer *asset_exposure = static_cast<MaterialPaintLayer *>(
      asset->paint_layers.first);
  MaterialPaintLayer *asset_contrast = asset_exposure->next;
  ASSERT_NE(asset_exposure, nullptr);
  ASSERT_NE(asset_contrast, nullptr);
  const PaintLayerKindInfo *chain_kind = paint_layer_kind_of(*chain);
  ASSERT_NE(chain_kind, nullptr);
  /* The aggregate is the members' schemas in stack order. Read them instead of a literal count:
   * a kind's parameter list is the branch's own business, and the reference the patch was cut from
   * may carry a different number of them. */
  const PaintLayerKindInfo *exposure_kind = paint_layer_kind_of(*asset_exposure);
  const PaintLayerKindInfo *contrast_kind = paint_layer_kind_of(*asset_contrast);
  ASSERT_NE(exposure_kind, nullptr);
  ASSERT_NE(contrast_kind, nullptr);
  EXPECT_EQ(chain_kind->params_num, exposure_kind->params_num + contrast_kind->params_num)
      << "the aggregate schema is the members' parameters in stack order";
  const std::string exposure_key = paint_layer_chain_member_param_key(asset_exposure->marker,
                                                                      "exposure");
  const std::string contrast_key = paint_layer_chain_member_param_key(asset_contrast->marker,
                                                                      "contrast");
  const int exposure_index = paint_layer_kind_param_index(*chain, exposure_key.c_str());
  const int contrast_index = paint_layer_kind_param_index(*chain, contrast_key.c_str());
  ASSERT_GE(exposure_index, 0);
  ASSERT_GE(contrast_index, 0);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*chain, exposure_index), 1.5f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*chain, contrast_index), 0.75f);

  /* The seeded parameters are offered to the instance, the way an import marks its copies. */
  EXPECT_TRUE(BKE_paint_layers_item_param_is_offered(*chain, exposure_key.c_str()));
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*chain, exposure_key.c_str()));
  /* An aggregate label reads "<member> · <parameter>", the descriptor's own wording. */
  ASSERT_LT(exposure_index, chain_kind->params_num);
  EXPECT_STREQ(chain_kind->params[exposure_index].label, "Exposure \xc2\xb7 Exposure");
}

TEST_F(PaintLayersChainTest, smart_filter_unfolds_into_the_corrections_the_members_describe)
{
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};

  /* The manual answer: a row carrying the same two pointwise filters, values and all. */
  Material *manual = BKE_material_add(bmain, "Manual");
  MaterialPaintLayer *manual_row = add_fill_row(*manual, "Row", white);
  add_pointwise_members(*manual, *manual_row);
  Vector<PaintMaterialCompositeImageLayer> manual_layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *manual, PAINT_MATERIAL_CHANNEL_BASE_COLOR, manual_layers));
  ASSERT_EQ(manual_layers.size(), 1);
  ASSERT_EQ(manual_layers[0].content_corrections.size(), 2);
  EXPECT_EQ(manual_layers[0].content_corrections[0].filter, MA_PAINT_LAYER_FILTER_EXPOSURE);
  EXPECT_EQ(manual_layers[0].content_corrections[1].filter,
            MA_PAINT_LAYER_FILTER_CONTRAST_LUMINOSITY);

  /* The chain answer: the same members saved as a Smart Filter and inserted on the same shape of
   * row, laid as one chain instance. */
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *saved_row = add_fill_row(*src, "SavedRow", white);
  const ChainMembers members = add_pointwise_members(*src, *saved_row);
  const MaterialPaintLayer *member_rows[2] = {members.exposure, members.contrast};
  Material *asset = BKE_paint_layers_asset_create(
      *bmain, *src, Span<const MaterialPaintLayer *>(member_rows, 2), PaintLayerAssetType::Filter,
      "ParityChain");
  ASSERT_NE(asset, nullptr);

  Material *chained = BKE_material_add(bmain, "Chained");
  MaterialPaintLayer *chained_row = add_fill_row(*chained, "Row", white);
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_asset_insert(
      *bmain, *chained, *asset, chained_row, PaintLayerPlace::Above, &result));
  ASSERT_NE(chain_effect_of(*chained_row), nullptr);

  Vector<PaintMaterialCompositeImageLayer> chain_layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *chained, PAINT_MATERIAL_CHANNEL_BASE_COLOR, chain_layers));
  ASSERT_EQ(chain_layers.size(), 1);
  /* The chain lays the members, not itself: as many corrections as the manual row carries, in the
   * definition's stack order, the same kinds. */
  ASSERT_EQ(chain_layers[0].content_corrections.size(), 2);
  EXPECT_EQ(chain_layers[0].content_corrections[0].filter, MA_PAINT_LAYER_FILTER_EXPOSURE);
  EXPECT_EQ(chain_layers[0].content_corrections[1].filter,
            MA_PAINT_LAYER_FILTER_CONTRAST_LUMINOSITY);

  /* Every correction matches the manual one: opacity and the whole packed parameter array, the
   * instance's overrides included (the exposure member's 1.5 rode the save into the seed). */
  for (const int i : IndexRange(2)) {
    const PaintMaterialCompositeCorrection &from_chain = chain_layers[0].content_corrections[i];
    const PaintMaterialCompositeCorrection &by_hand = manual_layers[0].content_corrections[i];
    EXPECT_FLOAT_EQ(from_chain.opacity, by_hand.opacity) << i;
    for (const int p : IndexRange(PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM)) {
      EXPECT_FLOAT_EQ(from_chain.filter_params[p], by_hand.filter_params[p])
          << "correction " << i << " param " << p;
    }
    EXPECT_EQ(from_chain.filter_ramp, by_hand.filter_ramp) << i;
    EXPECT_EQ(from_chain.filter_curves, by_hand.filter_curves) << i;
  }
  /* One overridden value, spelled out: the exposure the member was saved with, not the default. */
  EXPECT_FLOAT_EQ(chain_layers[0].content_corrections[0].filter_params
                      [PAINT_LAYER_EXPOSURE_EXPOSURE],
                  1.5f);
}

TEST_F(PaintLayersChainTest, chain_instance_edits_follow_the_definition_without_a_member_touch)
{
  /* An exposed value edited on the instance moves the override; the definition's member keeps its
   * own, and a second instance of the same asset reads the member's value again. */
  Material *src = BKE_material_add(bmain, "Src");
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  MaterialPaintLayer *row = add_fill_row(*src, "Row", white);
  const ChainMembers members = add_pointwise_members(*src, *row);
  const MaterialPaintLayer *member_rows[1] = {members.exposure};

  Material *asset = BKE_paint_layers_asset_create(
      *bmain, *src, Span<const MaterialPaintLayer *>(member_rows, 1), PaintLayerAssetType::Filter,
      "TwoInstances");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *first_target = add_fill_row(*dst, "First", white);
  MaterialPaintLayer *second_target = add_fill_row(*dst, "Second", white);
  PaintLayerImportResult first_result;
  ASSERT_TRUE(BKE_paint_layers_asset_insert(
      *bmain, *dst, *asset, first_target, PaintLayerPlace::Above, &first_result));
  PaintLayerImportResult second_result;
  ASSERT_TRUE(BKE_paint_layers_asset_insert(
      *bmain, *dst, *asset, second_target, PaintLayerPlace::Above, &second_result));

  MaterialPaintLayer *first_chain = chain_effect_of(*first_target);
  MaterialPaintLayer *second_chain = chain_effect_of(*second_target);
  ASSERT_NE(first_chain, nullptr);
  ASSERT_NE(second_chain, nullptr);

  MaterialPaintLayer *asset_exposure = static_cast<MaterialPaintLayer *>(
      asset->paint_layers.first);
  ASSERT_NE(asset_exposure, nullptr);
  const std::string exposure_key = paint_layer_chain_member_param_key(asset_exposure->marker,
                                                                      "exposure");
  const int first_index = paint_layer_kind_param_index(*first_chain, exposure_key.c_str());
  ASSERT_GE(first_index, 0);
  /* Both instances start at the member's saved value. */
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*first_chain, first_index), 1.5f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*second_chain, first_index), 1.5f);

  /* The first instance moves its own; the second and the definition keep theirs. */
  ASSERT_TRUE(paint_layer_kind_param_set(*first_chain, first_index, 0.25f));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*first_chain, first_index), 0.25f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*second_chain, first_index), 1.5f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(
                      *asset_exposure, PAINT_LAYER_EXPOSURE_EXPOSURE),
                  1.5f);
}

/**
 * A definition with a GPU-computed member (a Blur after a pointwise Exposure), saved, inserted, and
 * described: the pointwise prefix rides the plain filter path, and the member is an effect
 * correction carrying the composite marker (distinct from the member's own DNA marker), the
 * instance-unique hash and the chain/member pair the GPU job walk resolves it by -- with the
 * result computed through the member's cache, and the prefix describe the GPU job lays from
 * folding the members before it describe-only.
 */
TEST_F(PaintLayersChainTest, chain_with_a_gpu_member_unfolds_an_effect_correction_and_its_prefix)
{
  const int kChannel = int(PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* The definition: a pointwise Exposure the chain lays as a filter, then the Blur the chain lays
   * as an effect correction of its own. */
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *saved_row = add_map_row(*src, "SavedRow", add_ramp_image("SavedMap"));
  MaterialPaintLayer *exposure = BKE_paint_layers_filter_add(
      *src, saved_row, MA_PAINT_LAYER_FILTER_EXPOSURE, "Exposure");
  ASSERT_NE(exposure, nullptr);
  MaterialPaintLayer *blur = BKE_paint_layers_filter_add(
      *src, saved_row, MA_PAINT_LAYER_FILTER_BLUR, "Blur");
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 2));
  const MaterialPaintLayer *member_rows[2] = {exposure, blur};
  Material *asset = BKE_paint_layers_asset_create(
      *bmain, *src, Span<const MaterialPaintLayer *>(member_rows, 2), PaintLayerAssetType::Filter,
      "GpuMemberChain");
  ASSERT_NE(asset, nullptr);

  Material *chained = BKE_material_add(bmain, "Chained");
  MaterialPaintLayer *target = add_map_row(*chained, "Target", add_ramp_image("TargetMap"));
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_asset_insert(
      *bmain, *chained, *asset, target, PaintLayerPlace::Above, &result));
  MaterialPaintLayer *chain = chain_effect_of(*target);
  ASSERT_NE(chain, nullptr);
  MaterialPaintLayer *member = definition_member_of_kind(*chain, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(member, nullptr);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*chained, kChannel, layers));
  ASSERT_EQ(layers.size(), 1);
  ASSERT_EQ(layers[0].content_corrections.size(), 2);
  /* The pointwise prefix rides the plain path: a filter correction with no marker, exactly what a
   * hand-added Exposure describes. */
  EXPECT_EQ(layers[0].content_corrections[0].filter, MA_PAINT_LAYER_FILTER_EXPOSURE);
  EXPECT_TRUE(BLI_uuid_is_nil(layers[0].content_corrections[0].effect_marker));
  /* The GPU member is an effect correction: the member's kind, the composite marker (non-nil, and
   * no marker any DNA row of either material carries), the instance-unique hash, and the pair the
   * marker stands for. */
  const PaintMaterialCompositeCorrection &effect = layers[0].content_corrections[1];
  EXPECT_EQ(effect.filter, MA_PAINT_LAYER_FILTER_BLUR);
  EXPECT_FALSE(BLI_uuid_is_nil(effect.effect_marker));
  EXPECT_FALSE(BLI_uuid_equal(effect.effect_marker, member->marker));
  EXPECT_NE(effect.effect_hash, 0ull);
  EXPECT_EQ(effect.effect_member, member);
  EXPECT_EQ(effect.effect_chain, chain);
  /* The composite computed the result: the cache's answer, not a describe-only stub. */
  EXPECT_NE(effect.effect_color, nullptr);
  EXPECT_FLOAT_EQ(effect.opacity,
                  BKE_paint_layers_channel_opacity_effective(*member, kChannel));

  /* The prefix describe the GPU job lays from: the chain's input with the Exposure folded in, the
   * member's own correction describe-only, and no mask (the input describe clears the row's). */
  PaintMaterialCompositeImageLayer entry;
  ASSERT_TRUE(BKE_paint_layers_chain_member_input_describe(*chained,
                                                           *target,
                                                           *chain,
                                                           *member,
                                                           kChannel,
                                                           /*dimensions_only=*/false,
                                                           /*describe_only=*/true,
                                                           entry));
  EXPECT_TRUE(entry.mask_items.is_empty());
  EXPECT_EQ(entry.mask_image, nullptr);
  ASSERT_EQ(entry.content_corrections.size(), 2);
  EXPECT_EQ(entry.content_corrections[0].filter, MA_PAINT_LAYER_FILTER_EXPOSURE);
  EXPECT_EQ(entry.content_corrections[1].filter, MA_PAINT_LAYER_FILTER_BLUR);
  EXPECT_TRUE(BLI_uuid_equal(entry.content_corrections[1].effect_marker, effect.effect_marker));
  EXPECT_EQ(entry.content_corrections[1].effect_color, nullptr)
      << "describe-only names the maps and computes nothing";
}

/**
 * A chain whose definition holds a GPU-computed member computes exactly what the stack the user
 * would have built by hand computes: the frame the composite lays from the chain (the prefix as a
 * filter, the member as an effect correction over the chain's content with the prefix folded in)
 * matches the frame the same members laid as effects of the row, and the member's cache answer --
 * the one the GPU preview's baked member uploads -- is the manual blur's own, texel for texel.
 */
TEST_F(PaintLayersChainTest, chain_gpu_member_result_matches_the_manual_stack)
{
  const int kChannel = int(PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* The manual answer: the same members as effects of the row, values and all. */
  Material *manual = BKE_material_add(bmain, "Manual");
  MaterialPaintLayer *manual_row = add_map_row(*manual, "Row", add_ramp_image("ManualMap"));
  MaterialPaintLayer *manual_exposure = BKE_paint_layers_filter_add(
      *manual, manual_row, MA_PAINT_LAYER_FILTER_EXPOSURE, "Exposure");
  ASSERT_NE(manual_exposure, nullptr);
  MaterialPaintLayer *manual_blur = BKE_paint_layers_filter_add(
      *manual, manual_row, MA_PAINT_LAYER_FILTER_BLUR, "Blur");
  ASSERT_NE(manual_blur, nullptr);
  ASSERT_TRUE(paint_layer_kind_param_set(*manual_exposure, PAINT_LAYER_EXPOSURE_EXPOSURE, 1.5f));
  ASSERT_TRUE(
      paint_layer_kind_param_int_set(*manual_blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*manual_blur, PAINT_LAYER_BLUR_RADIUS, 2));

  /* The chained answer: the same members saved as a Smart Filter and inserted on the same shape of
   * row. */
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *saved_row = add_map_row(*src, "SavedRow", add_ramp_image("SavedMap"));
  MaterialPaintLayer *exposure = BKE_paint_layers_filter_add(
      *src, saved_row, MA_PAINT_LAYER_FILTER_EXPOSURE, "Exposure");
  ASSERT_NE(exposure, nullptr);
  MaterialPaintLayer *blur = BKE_paint_layers_filter_add(
      *src, saved_row, MA_PAINT_LAYER_FILTER_BLUR, "Blur");
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(paint_layer_kind_param_set(*exposure, PAINT_LAYER_EXPOSURE_EXPOSURE, 1.5f));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 2));
  const MaterialPaintLayer *member_rows[2] = {exposure, blur};
  Material *asset = BKE_paint_layers_asset_create(
      *bmain, *src, Span<const MaterialPaintLayer *>(member_rows, 2), PaintLayerAssetType::Filter,
      "ParityGpuChain");
  ASSERT_NE(asset, nullptr);

  Material *chained = BKE_material_add(bmain, "Chained");
  MaterialPaintLayer *target = add_map_row(*chained, "Row", add_ramp_image("TargetMap"));
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_asset_insert(
      *bmain, *chained, *asset, target, PaintLayerPlace::Above, &result));
  MaterialPaintLayer *chain = chain_effect_of(*target);
  ASSERT_NE(chain, nullptr);
  MaterialPaintLayer *member = definition_member_of_kind(*chain, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(member, nullptr);

  /* The whole frames match, the ramp's blurred edge included: the chain's prefix lays the same
   * exposure the manual row's own effect lays, and the member's correction lays the same blur over
   * it. The descriptions are built from the same shapes and the same values, so the pictures are
   * one. */
  Vector<float> manual_pixels;
  int manual_width = 0;
  int manual_height = 0;
  ASSERT_TRUE(render_base_color(*manual, manual_pixels, manual_width, manual_height));
  Vector<float> chained_pixels;
  int chained_width = 0;
  int chained_height = 0;
  ASSERT_TRUE(render_base_color(*chained, chained_pixels, chained_width, chained_height));
  ASSERT_EQ(manual_width, chained_width);
  ASSERT_EQ(manual_height, chained_height);
  for (const int64_t i : manual_pixels.index_range()) {
    EXPECT_NEAR(chained_pixels[i], manual_pixels[i], 1e-6f)
        << "the chain lays the members' picture, texel " << i / 4;
  }

  /* The member's cache answer is the manual blur's own: the input the member computed from is the
   * chain's content with the Exposure folded in, which is the row's content with the Exposure
   * laid -- the same input the manual blur describes up to its marker -- and the kernel is the
   * member's own, the same kind and parameters.
   *
   * Disabled until `06_combined_preview`, which adds
   * #BKE_paint_layers_effect_buffer_result: the parity above already compares the laid frames, and
   * this block only adds the buffer-level twin once that getter exists. */
#if 0
  int manual_buffer_width = 0;
  int manual_buffer_height = 0;
  std::shared_ptr<ImBuf> manual_buffer = BKE_paint_layers_effect_buffer_result(
      *manual, *manual_blur, kChannel, PAINT_TILE_NUMBER_BASE, manual_buffer_width,
      manual_buffer_height);
  ASSERT_NE(manual_buffer, nullptr);
  int member_buffer_width = 0;
  int member_buffer_height = 0;
  std::shared_ptr<ImBuf> member_buffer = BKE_paint_layers_chain_member_effect_buffer_result(
      *chained, *target, *chain, *member, kChannel, PAINT_TILE_NUMBER_BASE, member_buffer_width,
      member_buffer_height);
  ASSERT_NE(member_buffer, nullptr);
  EXPECT_EQ(manual_buffer_width, member_buffer_width);
  EXPECT_EQ(manual_buffer_height, member_buffer_height);
  ASSERT_NE(manual_buffer->float_data(), nullptr);
  ASSERT_NE(member_buffer->float_data(), nullptr);
  const int64_t total = int64_t(member_buffer_width) * member_buffer_height * 4;
  for (const int64_t i : IndexRange(total)) {
    EXPECT_NEAR(member_buffer->float_data()[i], manual_buffer->float_data()[i], 1e-6f)
        << "the member's answer is the manual blur's, texel " << i / 4;
  }
#endif
}

/**
 * Two instances of one asset in one material carry the same definition member -- the same kind,
 * the same parameters, the same DNA marker -- so the composite marker and the instance-unique hash
 * are what keep their cache entries apart: two chains, two corrections, two markers, two hashes,
 * two answers.
 */
TEST_F(PaintLayersChainTest, chain_gpu_member_keys_two_instances_of_one_asset_apart)
{
  const int kChannel = int(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};

  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *saved_row = add_map_row(*src, "SavedRow", add_ramp_image("SavedMap"));
  MaterialPaintLayer *blur = BKE_paint_layers_filter_add(
      *src, saved_row, MA_PAINT_LAYER_FILTER_BLUR, "Blur");
  ASSERT_NE(blur, nullptr);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 2));
  const MaterialPaintLayer *member_rows[1] = {blur};
  Material *asset = BKE_paint_layers_asset_create(
      *bmain, *src, Span<const MaterialPaintLayer *>(member_rows, 1), PaintLayerAssetType::Filter,
      "TwoInstanceGpu");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  /* A small canvas: the flat row has no map to measure, so the chain's member buffer is made at
   * the material's size, and the test wants the same small scale as the map row beside it. */
  dst->paint_layers_image_size = 8;
  /* The two targets read different content, so the two answers differ in pixels as well as in
   * keys: a ramp map, and the flat field beside it. (The flat field is an image here: a Fill row's
   * content has no map to evaluate the member over yet.) */
  MaterialPaintLayer *first_target = add_map_row(*dst, "First", add_ramp_image("FirstMap"));
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *second_map = BKE_image_add_generated(
      bmain, 8, 8, "SecondMap", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(second_map, nullptr);
  MaterialPaintLayer *second_target = add_map_row(*dst, "Second", second_map);
  PaintLayerImportResult first_result;
  ASSERT_TRUE(BKE_paint_layers_asset_insert(
      *bmain, *dst, *asset, first_target, PaintLayerPlace::Above, &first_result));
  PaintLayerImportResult second_result;
  ASSERT_TRUE(BKE_paint_layers_asset_insert(
      *bmain, *dst, *asset, second_target, PaintLayerPlace::Above, &second_result));
  MaterialPaintLayer *first_chain = chain_effect_of(*first_target);
  MaterialPaintLayer *second_chain = chain_effect_of(*second_target);
  ASSERT_NE(first_chain, nullptr);
  ASSERT_NE(second_chain, nullptr);
  /* The same definition member, read through both chains: the pointer the description carries. */
  MaterialPaintLayer *member = definition_member_of_kind(*first_chain, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(member, nullptr);
  EXPECT_EQ(definition_member_of_kind(*second_chain, MA_PAINT_LAYER_FILTER_BLUR), member);

  Vector<PaintMaterialCompositeImageLayer> first_layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*dst, kChannel, first_layers));
  /* Both rows are described in the one stack walk: the corrections sit on their own rows. */
  ASSERT_EQ(first_layers.size(), 2);
  const PaintMaterialCompositeCorrection *first_effect = nullptr;
  const PaintMaterialCompositeCorrection *second_effect = nullptr;
  for (const PaintMaterialCompositeImageLayer &layer : first_layers) {
    for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
      if (correction.filter != MA_PAINT_LAYER_FILTER_BLUR) {
        continue;
      }
      if (first_effect == nullptr) {
        first_effect = &correction;
      }
      else {
        second_effect = &correction;
      }
    }
  }
  ASSERT_NE(first_effect, nullptr);
  ASSERT_NE(second_effect, nullptr);
  EXPECT_EQ(first_effect->effect_member, member);
  EXPECT_EQ(second_effect->effect_member, member);
  EXPECT_FALSE(BLI_uuid_equal(first_effect->effect_marker, second_effect->effect_marker))
      << "the composite marker folds the chain row's own marker, and the two chains differ";
  EXPECT_NE(first_effect->effect_hash, second_effect->effect_hash)
      << "the instance-unique hash folds the composite marker";

  /* The cache answers under the two keys: two entries, each the blur of its own row's content. */
  int first_width = 0;
  int first_height = 0;
  std::shared_ptr<ImBuf> first_buffer = BKE_paint_layers_chain_member_effect_buffer_result(
      *dst, *first_target, *first_chain, *member, kChannel, PAINT_TILE_NUMBER_BASE, first_width,
      first_height);
  ASSERT_NE(first_buffer, nullptr);
  int second_width = 0;
  int second_height = 0;
  std::shared_ptr<ImBuf> second_buffer = BKE_paint_layers_chain_member_effect_buffer_result(
      *dst, *second_target, *second_chain, *member, kChannel, PAINT_TILE_NUMBER_BASE, second_width,
      second_height);
  ASSERT_NE(second_buffer, nullptr);
  EXPECT_NE(first_buffer.get(), second_buffer.get())
      << "two instances of one asset key apart, whatever their member shares";
  bool differs = false;
  const int64_t count = int64_t(first_width) * first_height * 4;
  for (const int64_t i : IndexRange(count)) {
    if (std::abs(first_buffer->float_data()[i] - second_buffer->float_data()[i]) > 1e-6f) {
      differs = true;
      break;
    }
  }
  EXPECT_TRUE(differs) << "the blur of a ramp is not the blur of a flat field";
}

TEST_F(PaintLayersChainTest, freeing_a_definition_forgets_its_chain_descriptor)
{
  /* The chain descriptors are keyed on the definition material's session uid, so a definition
   * that dies must take its entry with it: without the material free's forget, every Smart Filter
   * a session dropped would leave its aggregate schema in the cache forever. */
  const int before = paint_layer_kind_dynamic_descriptor_count();

  for ([[maybe_unused]] const int i : IndexRange(10)) {
    Material *definition = BKE_material_add(bmain, "Definition To Forget");
    ASSERT_NE(definition, nullptr);
    MaterialPaintLayer *member_row = BKE_paint_layers_add(
        *definition, "Row", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(member_row, nullptr);
    ASSERT_NE(BKE_paint_layers_filter_add(*definition,
                                         member_row,
                                         MA_PAINT_LAYER_FILTER_EXPOSURE,
                                         "Exposure"),
             nullptr);

    Material *ma = BKE_material_add(bmain, "Instance");
    ASSERT_NE(ma, nullptr);
    MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "R", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(row, nullptr);
    MaterialPaintLayer *chain = BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_CHAIN);
    ASSERT_NE(chain, nullptr);
    /* Hand-wired the way the stack tests shape their rows: the descriptor is keyed on the
     * definition alone, not on how the link was made. */
    chain->material = definition;

    /* Reading the kind publishes the chain's aggregate schema, keyed on the definition's uid. */
    ASSERT_NE(paint_layer_kind_of(*chain), nullptr);

    /* The hand-wired link took no user, so it is dropped before the definition dies: the free
     * must not be followed by a walk of the instance's rows through a dangling pointer. */
    chain->material = nullptr;
    BKE_id_free(bmain, definition);
  }

  EXPECT_EQ(paint_layer_kind_dynamic_descriptor_count(), before)
      << "each definition's free retired the chain descriptor it was keyed on";
}

/**
 * The row's bake hash is what retires the bake, and a chain's pixels are its definition's: the
 * definition's shape and its members' own descriptions fold in beside the instance's overrides, so
 * every edit that can move the chain's result retires it while an unrelated material does not.
 */
TEST_F(PaintLayersChainTest, a_chain_bake_hash_folds_its_definition)
{
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *saved_row = add_fill_row(*src, "SavedRow", white);
  const ChainMembers members = add_pointwise_members(*src, *saved_row);
  const MaterialPaintLayer *member_rows[2] = {members.exposure, members.contrast};
  Material *asset = BKE_paint_layers_asset_create(
      *bmain, *src, Span<const MaterialPaintLayer *>(member_rows, 2), PaintLayerAssetType::Filter,
      "HashedChain");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *row = add_fill_row(*dst, "Row", white);
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_asset_insert(
      *bmain, *dst, *asset, row, PaintLayerPlace::Above, &result));
  MaterialPaintLayer *chain = chain_effect_of(*row);
  ASSERT_NE(chain, nullptr);

  uint32_t baseline[2] = {};
  BKE_paint_layers_bake_hash(*dst, *row, baseline);

  /* An unrelated material's edit moves nothing: no row of it is read by this stack. */
  Material *unrelated = BKE_material_add(bmain, "Unrelated");
  MaterialPaintLayer *unrelated_row = add_fill_row(*unrelated, "UnrelatedRow", white);
  add_pointwise_members(*unrelated, *unrelated_row);
  uint32_t after_unrelated[2] = {};
  BKE_paint_layers_bake_hash(*dst, *row, after_unrelated);
  EXPECT_EQ(after_unrelated[0], baseline[0]);
  EXPECT_EQ(after_unrelated[1], baseline[1]);

  /* An exposed value edited on the instance retires the bake. */
  MaterialPaintLayer *asset_exposure = static_cast<MaterialPaintLayer *>(asset->paint_layers.first);
  ASSERT_NE(asset_exposure, nullptr);
  const std::string exposure_key = paint_layer_chain_member_param_key(asset_exposure->marker,
                                                                      "exposure");
  const int exposure_index = paint_layer_kind_param_index(*chain, exposure_key.c_str());
  ASSERT_GE(exposure_index, 0);
  ASSERT_TRUE(paint_layer_kind_param_set(*chain, exposure_index, 0.5f));
  uint32_t after_override[2] = {};
  BKE_paint_layers_bake_hash(*dst, *row, after_override);
  EXPECT_FALSE(after_override[0] == baseline[0] && after_override[1] == baseline[1])
      << "an instance override is part of what the chain lays";

  /* A member's own value, edited in the definition, retires it too: the instance carries no
   * override for a parameter the user never touched. */
  ASSERT_TRUE(paint_layer_kind_param_set(*asset_exposure, PAINT_LAYER_EXPOSURE_EXPOSURE, 2.0f));
  uint32_t after_member[2] = {};
  BKE_paint_layers_bake_hash(*dst, *row, after_member);
  EXPECT_FALSE(after_member[0] == after_override[0] && after_member[1] == after_override[1])
      << "a definition member's value is part of what the chain lays";
}

}  // namespace blender::bke::tests
