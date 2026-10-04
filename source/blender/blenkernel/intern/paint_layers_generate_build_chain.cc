#include "paint_layers_generate_intern.hh"
/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The paint-layer generator: `Material::paint_layers` (the DNA description) becomes a node tree.
 *
 * `paint_layers_tree_build` is the topology half: interface, per-layer nodes and the links between
 * them, with no #Main involved. `BKE_paint_layers_regenerate` is the #Main-side half: it owns the
 * generated group, the instance node in the material's embedded tree, and the routing into the
 * Principled BSDF.
 *
 * Topology and values are separated: the channel chains and the interface are a function of the
 * description's *structure* alone, while the animatable values (`opacity`, `enabled`, a Fill
 * constant) are inputs of each layer group's own interface. Their current values sit on that
 * group's instance node in its parent tree and are copied there by #BKE_paint_layers_values_sync
 * (called from the material evaluation too).
 */

#include "BKE_paint_layers_debug.hh"
#include "BKE_paint_layers_generate.hh"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_threads.h"
#include "BLI_time.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "IMB_colormanagement.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "paint_layers_intern.hh"

#include "paint_layers_generate_build_intern.hh"

#include "NOD_socket.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "IMB_imbuf_types.hh"

#include "paint_material_composite_internal.hh"

/* The recursive chain builders: #PaintLayersChainBuilder::build_list and its row
 * builder. */

namespace blender {
namespace bke::paint_layers {

ChainResult PaintLayersChainBuilder::build_list(
    const ListBaseT<MaterialPaintLayer> &list,
    ChainLayer previous,
    const bool premul,
    const RowTarget &parent_target,
    const int channel)
{
  const Material &ma = outer_.ma_;
  const PaintLayersRegenCache *const cache = outer_.cache_;
  const PaintLayersBuildContext &ctx = outer_.ctx_;
  float &location_x = location_x_;
  const float location_y = location_y_;
  /* Shadows the enclosing per-channel loop's own `channel` for this whole call and everything
   * it recurses into (including the nested #build_row below): a Stack mask's subtree builds in
   * its own fixed `mask_channel` (or Base Color, for the Alpha convention), never in whichever
   * channel the owner row happens to be generating right now -- see the two call sites below
   * that pass something other than the plain `channel` they were handed. #track_content_alpha
   * is re-derived from this parameter for the same reason: Normal is the only channel that does
   * not track it, and a mask fixed to a non-Normal channel must track it even while the owner
   * itself is generating its own Normal row. */
  const bool track_content_alpha =
      BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel(channel));
  /* This list builds in the tree it is handed: the root, or a folder's own group. */
  bNodeTree &tree = *parent_target.tree;
  bNode *coverage_node = nullptr;
  bNodeSocket *coverage = nullptr;
  bNode *content_alpha_node = nullptr;
  bNodeSocket *content_alpha = nullptr;
  if (premul) {
    bNode *zero = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
    if (zero != nullptr) {
      coverage_node = zero;
      coverage = socket_out(*zero, "Value");
      if (coverage != nullptr && coverage->default_value != nullptr) {
        static_cast<bNodeSocketValueFloat *>(coverage->default_value)->value = 0.0f;
      }
    }
  }
  for (const MaterialPaintLayer &layer_ref :
       list)
  {
    const MaterialPaintLayer *layer = &layer_ref;
    if (BKE_paint_layers_folder_is_pass_through(ma, *layer))
    {
      if (row_is_removed(ma, *layer)) {
        /* The over-budget pass dropped this hidden folder; its inlined subtree goes with it. */
        continue;
      }
      /* A Pass Through folder is expanded in place: its children are built straight into the
       * parent's chain with the parent's own premul mode, exactly as if the folder were not
       * there. This is what keeps the generated shader identical when a row is moved into or
       * out of such a folder.
       *
       * Why inlining and not an isolated sub-chain: EEVEE compiles a material as one shader, so
       * any structural difference in the generated graph recompiles it; with a live Material row
       * carrying a large source graph that is seconds of stall. The isolated folder always edits
       * that structure (its own group instance, the P/a divide, the coverage overlay), while the
       * inlined form does not, so EEVEE can take the unchanged pass from its cache.
       *
       * Why it is correct: over is associative under Normal/Mix at full opacity with no mask,
       * correction or bake, so `over(below, P/a, a)` of the isolated folder equals laying the
       * children over `below` one after another. The predicate rejects every case where that
       * fails (opacity below one with several children in particular); a hidden Pass Through
       * folder scales each descendant's factor to zero through its value input, which is exact
       * and stays a value edit.
       *
       * What it costs and what to check when refactoring: switching a folder's mode is one
       * rebuild and one EEVEE compile, which is acceptable. The win depends on the build
       * producing byte-identical node and link order before and after a move; if it stops doing
       * so the compile returns silently. The CPU compositor and #BKE_paint_layers_bake_render_node
       * must keep giving the GPU's result -- see `composite_image_layers_build`, which flattens
       * the same folder. Alternatives considered and rejected: reserving correction slots (does
       * not avoid the code change) and an always-present opacity input (complicates every chain
       * for this rare case). */
      ChainResult sub = build_list(layer->children, previous, premul, parent_target, channel);
      previous = sub.chain;
      coverage_node = sub.coverage_node;
      coverage = sub.coverage;
      content_alpha_node = sub.content_alpha_node;
      content_alpha = sub.content_alpha;
      continue;
    }
    Image *baked_color = nullptr;
    const bool substituted = row_channel_substituted(ma, *layer, channel, &baked_color);

    RowTarget row_target = parent_target;
    row_target.location_x = location_x;
    row_target.location_y = location_y;
    LayerGroup *layer_group = nullptr;
    /* Every row gets its own group: a bake-substituted row, a folder that takes part, or a leaf
     * that paints something here. The same helper feeds the root hash, so the root and the build
     * never disagree about which instances exist. */
    const bool group_this = layer_row_has_group(ma, *layer, channel, cache);
    if (group_this) {
      layer_group = outer_.layer_group_ensure(*layer, *parent_target.tree);
    }
    RowResult row;
    if (layer_group != nullptr && layer_group->unchanged) {
      /* The group's topology is current: keep its nodes and interface and chain the existing
       * instance. */
      row = outer_.row_from_unchanged_group(*layer_group, *layer, channel);
    }
    else {
      if (layer_group != nullptr) {
        row_target.tree = layer_group->tree;
        row_target.group_input = layer_group->group_input;
      }
      row = build_row(layer, row_target, substituted, baked_color, premul, layer_group, channel);
    }
    if (!row.valid) {
      /* [PL-DIAG] Row-chain audit for the correction-visibility report: which rows take part
       * per channel and how (live or bake-substituted). Prints only. Compare the Layer 1
       * lines between the one-row and two-row states: identical lines prove the graph side. */
      printf("[PL-DIAG] chain row='%s' channel=%d substituted=%d grouped=%d valid=0\n",
             layer->name,
             channel,
             int(substituted),
             int(layer_group != nullptr));
      continue;
    }
    /* [PL-DIAG] See above: the taking-part counterpart of the skip line. */
    printf("[PL-DIAG] chain row='%s' channel=%d substituted=%d grouped=%d valid=1\n",
           layer->name,
           channel,
           int(substituted),
           int(layer_group != nullptr));
    ChainLayer current = row.current;
    if (row.grouped) {
      /* The parent chains the group through its instance: Color and Coverage are the row's
       * straight result, while Below/Blend/Result are the instance's own sockets. */
      current.source_node = row.group_instance;
      current.source = row.group_color;
      current.factor_node = row.group_instance;
      current.factor = row.group_coverage;
      /* The row's straight content alpha comes from the instance's Content Alpha output when
       * the group tracked one; an absent output leaves the chain untracked (treat as 1.0).
       * The inner-tree socket is never kept here, it lives in another tree. */
      current.content_alpha_node = (row.group_content_alpha != nullptr) ? row.group_instance :
                                                                          nullptr;
      current.content_alpha = row.group_content_alpha;
    }

  if (premul) {
    /* The isolated-group accumulation (design §5): S = P/a, c_eff = lerp(c, blend(S, c), a),
     * P = P(1-f) + c_eff*f, a = a(1-f) + f. */
    bNode *s_divide = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
    bNodeSocket *sd_a = (s_divide != nullptr) ? socket_in(*s_divide, "Vector") : nullptr;
    bNodeSocket *sd_b = (s_divide != nullptr) ? socket_in(*s_divide, "Vector_001") : nullptr;
    bNodeSocket *sd_out = (s_divide != nullptr) ? socket_out(*s_divide, "Vector") : nullptr;
    if (sd_out == nullptr || coverage_node == nullptr || coverage == nullptr) {
      continue;
    }
    s_divide->custom1 = NODE_VECTOR_MATH_DIVIDE;
    s_divide->location[0] = location_x;
    s_divide->location[1] = location_y + 120.0f;
    bke::node_add_link(tree, *previous.source_node, *previous.source, *s_divide, *sd_a);
    bke::node_add_link(tree, *coverage_node, *coverage, *s_divide, *sd_b);

    /* blend_m(S, c) at full factor. */
    bNode *row_blend = nullptr;
    bNodeSocket *row_blend_out = nullptr;
    if (row.grouped) {
      /* S = P/a feeds the group's Below; its Blend output is blend_m(S, Color) at factor one. */
      if (row.group_below != nullptr) {
        bke::node_add_link(tree, *s_divide, *sd_out, *row.group_instance, *row.group_below);
      }
      row_blend = row.group_instance;
      row_blend_out = row.group_blend;
    }
    else if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && ctx.normal_combine_group != nullptr &&
             !BKE_paint_layers_normal_replace(*current.layer) && tree.typeinfo != nullptr &&
             tree.typeinfo->group_idname != nullptr)
    {
      bNode *combine = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
      if (combine != nullptr) {
        combine->id = &ctx.normal_combine_group->id;
        id_us_plus(&ctx.normal_combine_group->id);
        combine->location[0] = location_x;
        combine->location[1] = location_y - 120.0f;
        nodes::update_node_declaration_and_sockets(tree, *combine);
        bNodeSocket *a_in = bke::node_find_socket(
            *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
        bNodeSocket *b_in = bke::node_find_socket(
            *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
        bNodeSocket *f_in = bke::node_find_socket(
            *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
        bNodeSocket *r_out = bke::node_find_socket(
            *combine, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
        if (a_in != nullptr && b_in != nullptr && f_in != nullptr && r_out != nullptr) {
          if (f_in->default_value != nullptr) {
            static_cast<bNodeSocketValueFloat *>(f_in->default_value)->value = 1.0f;
          }
          bke::node_add_link(tree, *s_divide, *sd_out, *combine, *a_in);
          bke::node_add_link(tree, *current.source_node, *current.source, *combine, *b_in);
          row_blend = combine;
          row_blend_out = r_out;
        }
      }
    }
    else {
      bNode *mix = mix_node_add(tree,
                                BKE_paint_layers_blend_to_ramp(
                                    eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(*current.layer, channel))),
                                location_x,
                                location_y - 120.0f);
      if (mix != nullptr) {
        bNodeSocket *fac = socket_in(*mix, "Factor_Float");
        bNodeSocket *color1 = socket_in(*mix, "A_Color");
        bNodeSocket *color2 = socket_in(*mix, "B_Color");
        bNodeSocket *color_out = socket_out(*mix, "Result_Color");
        if (fac != nullptr && color1 != nullptr && color2 != nullptr && color_out != nullptr) {
          if (fac->default_value != nullptr) {
            static_cast<bNodeSocketValueFloat *>(fac->default_value)->value = 1.0f;
          }
          bke::node_add_link(tree, *s_divide, *sd_out, *mix, *color1);
          bke::node_add_link(tree, *current.source_node, *current.source, *mix, *color2);
          row_blend = mix;
          row_blend_out = color_out;
        }
      }
    }
    if (row_blend == nullptr || row_blend_out == nullptr) {
      continue;
    }

    /* c_eff = lerp(c, blend, a). */
    bNode *c_eff = mix_node_add(tree, MA_RAMP_BLEND, location_x + 120.0f, location_y - 240.0f);
    bNodeSocket *eff_a = (c_eff != nullptr) ? socket_in(*c_eff, "A_Color") : nullptr;
    bNodeSocket *eff_b = (c_eff != nullptr) ? socket_in(*c_eff, "B_Color") : nullptr;
    bNodeSocket *eff_f = (c_eff != nullptr) ? socket_in(*c_eff, "Factor_Float") : nullptr;
    bNodeSocket *eff_out = (c_eff != nullptr) ? socket_out(*c_eff, "Result_Color") : nullptr;
    if (eff_out == nullptr) {
      continue;
    }
    bke::node_add_link(tree, *current.source_node, *current.source, *c_eff, *eff_a);
    bke::node_add_link(tree, *row_blend, *row_blend_out, *c_eff, *eff_b);
    bke::node_add_link(tree, *coverage_node, *coverage, *c_eff, *eff_f);

    /* P = lerp(P, c_eff, f). */
    bNode *p_mix = mix_node_add(tree, MA_RAMP_BLEND, location_x + 240.0f, location_y);
    bNodeSocket *pm_a = (p_mix != nullptr) ? socket_in(*p_mix, "A_Color") : nullptr;
    bNodeSocket *pm_b = (p_mix != nullptr) ? socket_in(*p_mix, "B_Color") : nullptr;
    bNodeSocket *pm_f = (p_mix != nullptr) ? socket_in(*p_mix, "Factor_Float") : nullptr;
    bNodeSocket *pm_out = (p_mix != nullptr) ? socket_out(*p_mix, "Result_Color") : nullptr;
    if (pm_out == nullptr) {
      continue;
    }
    bke::node_add_link(tree, *previous.source_node, *previous.source, *p_mix, *pm_a);
    bke::node_add_link(tree, *c_eff, *eff_out, *p_mix, *pm_b);
    if (current.factor != nullptr && current.factor_node != nullptr) {
      bke::node_add_link(tree, *current.factor_node, *current.factor, *p_mix, *pm_f);
    }
    /* The content alpha beside the colour's own alpha: S_a = P_a / a through a Math divide
     * (safe on zero, next to the Vector divide above), then c_eff_a and P_a like the colour
     * path: c_eff_a = c_a at Mix, folded with the straight alpha below otherwise, and
     * P_a = P_a + (c_eff_a - P_a) * f. An untracked row counts as opaque; with no tracked
     * alpha anywhere nothing is built and the chain stays null. */
    bNode *content_pa_node = previous.content_alpha_node;
    bNodeSocket *content_pa = previous.content_alpha;
    if (track_content_alpha &&
        (previous.content_alpha != nullptr || current.content_alpha != nullptr))
    {
      bNode *below_node = previous.content_alpha_node;
      bNodeSocket *below = previous.content_alpha;
      if (below == nullptr) {
        below_node = coverage_node;
        below = coverage;
      }
      bNode *row_alpha_node = current.content_alpha_node;
      bNodeSocket *row_a = current.content_alpha;
      bNode *row_one = nullptr;
      if (row_a == nullptr) {
        row_one = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
        row_a = (row_one != nullptr) ? socket_out(*row_one, "Value") : nullptr;
        if (row_a != nullptr && row_a->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(row_a->default_value)->value = 1.0f;
        }
        row_alpha_node = row_one;
      }
      bNode *factor_one = nullptr;
      bNode *factor_node = current.factor_node;
      bNodeSocket *factor = current.factor;
      if (factor == nullptr) {
        factor_one = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
        factor = (factor_one != nullptr) ? socket_out(*factor_one, "Value") : nullptr;
        if (factor != nullptr && factor->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(factor->default_value)->value = 1.0f;
        }
        factor_node = factor_one;
      }
      bNode *alpha_div = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
      bNodeSocket *ad_a = (alpha_div != nullptr) ? socket_in(*alpha_div, "Value") : nullptr;
      bNodeSocket *ad_b = (alpha_div != nullptr) ? socket_in(*alpha_div, "Value_001") :
                                                   nullptr;
      bNodeSocket *ad_out = (alpha_div != nullptr) ? socket_out(*alpha_div, "Value") : nullptr;
      const int row_ramp = BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(
          BKE_paint_layers_channel_blend_effective(*current.layer, channel)));
      if (below != nullptr && below_node != nullptr && row_a != nullptr &&
          row_alpha_node != nullptr && factor != nullptr && factor_node != nullptr &&
          ad_out != nullptr)
      {
        alpha_div->custom1 = NODE_MATH_DIVIDE;
        alpha_div->location[0] = location_x + 240.0f;
        alpha_div->location[1] = location_y - 520.0f;
        bke::node_add_link(tree, *below_node, *below, *alpha_div, *ad_a);
        bke::node_add_link(tree, *coverage_node, *coverage, *alpha_div, *ad_b);
        /* c_eff_a: the row's own alpha at Mix, folded with the straight alpha below when the
         * row's blend leaves alpha (every mode but Mix). */
        bNode *ceff_node = row_alpha_node;
        bNodeSocket *ceff = row_a;
        if (row_ramp != MA_RAMP_BLEND) {
          bNode *csub = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
          bNode *cmul = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
          bNode *cadd = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
          bNodeSocket *cs_a = (csub != nullptr) ? socket_in(*csub, "Value") : nullptr;
          bNodeSocket *cs_b = (csub != nullptr) ? socket_in(*csub, "Value_001") : nullptr;
          bNodeSocket *cm_a = (cmul != nullptr) ? socket_in(*cmul, "Value") : nullptr;
          bNodeSocket *cm_b = (cmul != nullptr) ? socket_in(*cmul, "Value_001") : nullptr;
          bNodeSocket *ca_a = (cadd != nullptr) ? socket_in(*cadd, "Value") : nullptr;
          bNodeSocket *ca_b = (cadd != nullptr) ? socket_in(*cadd, "Value_001") : nullptr;
          bNodeSocket *cs_out = (csub != nullptr) ? socket_out(*csub, "Value") : nullptr;
          bNodeSocket *cm_out = (cmul != nullptr) ? socket_out(*cmul, "Value") : nullptr;
          bNodeSocket *ca_out = (cadd != nullptr) ? socket_out(*cadd, "Value") : nullptr;
          if (cs_out != nullptr && cm_out != nullptr && ca_out != nullptr) {
            csub->custom1 = NODE_MATH_SUBTRACT;
            cmul->custom1 = NODE_MATH_MULTIPLY;
            cadd->custom1 = NODE_MATH_ADD;
            bke::node_add_link(tree, *alpha_div, *ad_out, *csub, *cs_a);
            bke::node_add_link(tree, *row_alpha_node, *row_a, *csub, *cs_b);
            bke::node_add_link(tree, *csub, *cs_out, *cmul, *cm_a);
            bke::node_add_link(tree, *coverage_node, *coverage, *cmul, *cm_b);
            bke::node_add_link(tree, *row_alpha_node, *row_a, *cadd, *ca_a);
            bke::node_add_link(tree, *cmul, *cm_out, *cadd, *ca_b);
            ceff_node = cadd;
            ceff = ca_out;
          }
        }
        /* P_a = P_a + (c_eff_a - P_a) * f. */
        bNode *psub = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *pmul = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *padd = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNodeSocket *ps_a = (psub != nullptr) ? socket_in(*psub, "Value") : nullptr;
        bNodeSocket *ps_b = (psub != nullptr) ? socket_in(*psub, "Value_001") : nullptr;
        bNodeSocket *pm_a2 = (pmul != nullptr) ? socket_in(*pmul, "Value") : nullptr;
        bNodeSocket *pm_b2 = (pmul != nullptr) ? socket_in(*pmul, "Value_001") : nullptr;
        bNodeSocket *pa_a = (padd != nullptr) ? socket_in(*padd, "Value") : nullptr;
        bNodeSocket *pa_b = (padd != nullptr) ? socket_in(*padd, "Value_001") : nullptr;
        bNodeSocket *ps_out = (psub != nullptr) ? socket_out(*psub, "Value") : nullptr;
        bNodeSocket *pm_out2 = (pmul != nullptr) ? socket_out(*pmul, "Value") : nullptr;
        bNodeSocket *pa_out = (padd != nullptr) ? socket_out(*padd, "Value") : nullptr;
        if (ps_out != nullptr && pm_out2 != nullptr && pa_out != nullptr) {
          psub->custom1 = NODE_MATH_SUBTRACT;
          pmul->custom1 = NODE_MATH_MULTIPLY;
          padd->custom1 = NODE_MATH_ADD;
          bke::node_add_link(tree, *ceff_node, *ceff, *psub, *ps_a);
          bke::node_add_link(tree, *below_node, *below, *psub, *ps_b);
          bke::node_add_link(tree, *psub, *ps_out, *pmul, *pm_a2);
          bke::node_add_link(tree, *factor_node, *factor, *pmul, *pm_b2);
          bke::node_add_link(tree, *below_node, *below, *padd, *pa_a);
          bke::node_add_link(tree, *pmul, *pm_out2, *padd, *pa_b);
          content_pa_node = padd;
          content_pa = pa_out;
        }
      }
    }
    previous = {current.layer,
                p_mix,
                pm_out,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                content_pa_node,
                content_pa};
    content_alpha_node = content_pa_node;
    content_alpha = content_pa;

    /* a = a + f*(1-a). */
    bNode *one_minus = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
    bNode *scaled = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
    bNode *joined = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
    bNodeSocket *om_a = (one_minus != nullptr) ? socket_in(*one_minus, "Value") : nullptr;
    bNodeSocket *om_b = (one_minus != nullptr) ? socket_in(*one_minus, "Value_001") : nullptr;
    bNodeSocket *sc_a = (scaled != nullptr) ? socket_in(*scaled, "Value") : nullptr;
    bNodeSocket *sc_b = (scaled != nullptr) ? socket_in(*scaled, "Value_001") : nullptr;
    bNodeSocket *jo_a = (joined != nullptr) ? socket_in(*joined, "Value") : nullptr;
    bNodeSocket *jo_b = (joined != nullptr) ? socket_in(*joined, "Value_001") : nullptr;
    if (om_b == nullptr || sc_a == nullptr || sc_b == nullptr || jo_a == nullptr ||
        jo_b == nullptr)
    {
      continue;
    }
    one_minus->custom1 = NODE_MATH_SUBTRACT;
    scaled->custom1 = NODE_MATH_MULTIPLY;
    joined->custom1 = NODE_MATH_ADD;
    if (om_a != nullptr && om_a->default_value != nullptr) {
      static_cast<bNodeSocketValueFloat *>(om_a->default_value)->value = 1.0f;
    }
    bke::node_add_link(tree, *coverage_node, *coverage, *one_minus, *om_b);
    if (current.factor != nullptr && current.factor_node != nullptr) {
      bke::node_add_link(tree, *current.factor_node, *current.factor, *scaled, *sc_a);
    }
    else if (sc_a->default_value != nullptr) {
      static_cast<bNodeSocketValueFloat *>(sc_a->default_value)->value = 1.0f;
    }
    bke::node_add_link(tree, *one_minus, *socket_out(*one_minus, "Value"), *scaled, *sc_b);
    bke::node_add_link(tree, *coverage_node, *coverage, *joined, *jo_a);
    bke::node_add_link(tree, *scaled, *socket_out(*scaled, "Value"), *joined, *jo_b);
    coverage_node = joined;
    coverage = socket_out(*joined, "Value");
  }
  else if (row.grouped) {
    /* The group's Result already blends the row over Below; the parent only feeds Below. */
    if (previous.source != nullptr && previous.source_node != nullptr &&
        row.group_below != nullptr)
    {
      bke::node_add_link(tree,
                         *previous.source_node,
                         *previous.source,
                         *row.group_instance,
                         *row.group_below);
    }
    /* The parent's own content chain lays the group's straight alpha over the alpha below,
     * at Mix; any other blend leaves the alpha below alone, like the colour path. */
    bNode *below_content_node = previous.content_alpha_node;
    bNodeSocket *below_content = previous.content_alpha;
    /* An untracked row counts as opaque, like the isolated chain's `row_one`: a MESH_MAP or
     * Material leaf exposes no Content Alpha output, yet the CPU still blends the row's full
     * coverage by the row factor, so the chain has to see a constant one here. */
    bNode *row_one = nullptr;
    bNodeSocket *row_content = current.content_alpha;
    bNode *row_content_node = current.content_alpha_node;
    if (track_content_alpha && row_content == nullptr) {
      row_one = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
      row_content = (row_one != nullptr) ? socket_out(*row_one, "Value") : nullptr;
      if (row_content != nullptr && row_content->default_value != nullptr) {
        static_cast<bNodeSocketValueFloat *>(row_content->default_value)->value = 1.0f;
      }
      row_content_node = row_one;
    }
    if (track_content_alpha && row_content != nullptr && row_content_node != nullptr)
    {
      if (below_content == nullptr) {
        bNode *bottom_one = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
        below_content = (bottom_one != nullptr) ? socket_out(*bottom_one, "Value") : nullptr;
        if (below_content != nullptr && below_content->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(below_content->default_value)->value = 1.0f;
        }
        below_content_node = bottom_one;
      }
      const int group_ramp = BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(
          BKE_paint_layers_channel_blend_effective(*current.layer, channel)));
      if (below_content != nullptr && group_ramp == MA_RAMP_BLEND) {
        bNode *gsub = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *gmul = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *gadd = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNodeSocket *gs_a = (gsub != nullptr) ? socket_in(*gsub, "Value") : nullptr;
        bNodeSocket *gs_b = (gsub != nullptr) ? socket_in(*gsub, "Value_001") : nullptr;
        bNodeSocket *gm_a = (gmul != nullptr) ? socket_in(*gmul, "Value") : nullptr;
        bNodeSocket *gm_b = (gmul != nullptr) ? socket_in(*gmul, "Value_001") : nullptr;
        bNodeSocket *ga_a = (gadd != nullptr) ? socket_in(*gadd, "Value") : nullptr;
        bNodeSocket *ga_b = (gadd != nullptr) ? socket_in(*gadd, "Value_001") : nullptr;
        bNodeSocket *gs_out = (gsub != nullptr) ? socket_out(*gsub, "Value") : nullptr;
        bNodeSocket *gm_out = (gmul != nullptr) ? socket_out(*gmul, "Value") : nullptr;
        bNodeSocket *ga_out = (gadd != nullptr) ? socket_out(*gadd, "Value") : nullptr;
        if (gs_out != nullptr && gm_out != nullptr && ga_out != nullptr) {
          gsub->custom1 = NODE_MATH_SUBTRACT;
          gmul->custom1 = NODE_MATH_MULTIPLY;
          gadd->custom1 = NODE_MATH_ADD;
          bke::node_add_link(
              tree, *row_content_node, *row_content, *gsub, *gs_a);
          bke::node_add_link(tree, *below_content_node, *below_content, *gsub, *gs_b);
          bke::node_add_link(tree, *gsub, *gs_out, *gmul, *gm_a);
          if (current.factor != nullptr && current.factor_node != nullptr) {
            bke::node_add_link(
                tree, *current.factor_node, *current.factor, *gmul, *gm_b);
          }
          else if (gm_b != nullptr && gm_b->default_value != nullptr) {
            static_cast<bNodeSocketValueFloat *>(gm_b->default_value)->value = 1.0f;
          }
          bke::node_add_link(tree, *below_content_node, *below_content, *gadd, *ga_a);
          bke::node_add_link(tree, *gmul, *gm_out, *gadd, *ga_b);
          below_content_node = gadd;
          below_content = ga_out;
        }
      }
    }
    previous = {current.layer,
                row.group_instance,
                row.group_result,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                below_content_node,
                below_content};
    content_alpha_node = below_content_node;
    content_alpha = below_content;
  }
  else {
    bool combined = false;
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && ctx.normal_combine_group != nullptr &&
        !BKE_paint_layers_normal_replace(*current.layer) && tree.typeinfo != nullptr &&
        tree.typeinfo->group_idname != nullptr)
    {
      bNode *combine = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
      if (combine != nullptr) {
        combine->id = &ctx.normal_combine_group->id;
        id_us_plus(&ctx.normal_combine_group->id);
        combine->location[0] = location_x;
        combine->location[1] = location_y;
        /* A hand-assigned group only grows its instance sockets once its declaration is built;
         * this instantiates them from the group's interface without needing #Main or a whole
         * tree update. */
        nodes::update_node_declaration_and_sockets(tree, *combine);
        bNodeSocket *socket_a = bke::node_find_socket(
            *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
        bNodeSocket *socket_b = bke::node_find_socket(
            *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
        bNodeSocket *factor = bke::node_find_socket(
            *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
        bNodeSocket *result = bke::node_find_socket(
            *combine, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
        if (socket_a == nullptr || socket_b == nullptr || factor == nullptr ||
            result == nullptr)
        {
          /* The sockets are the group's own interface; a missing one is a build error, not a
           * shape to paper over with a Mix that would silently flatten the relief. */
          BLI_assert_msg(false, "Normal Combine instance sockets were not instantiated");
          continue;
        }
        bke::node_add_link(
            tree, *previous.source_node, *previous.source, *combine, *socket_a);
        bke::node_add_link(
            tree, *current.source_node, *current.source, *combine, *socket_b);
        if (current.factor != nullptr && current.factor_node != nullptr) {
          bke::node_add_link(tree, *current.factor_node, *current.factor, *combine, *factor);
        }
        previous = {current.layer,
                    combine,
                    result,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    previous.content_alpha_node,
                    previous.content_alpha};
        combined = true;
      }
    }
    if (!combined) {
      bNode *mix = mix_node_add(tree,
                                BKE_paint_layers_blend_to_ramp(
                                    eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(*current.layer, channel))),
                                location_x,
                                location_y);
      if (mix != nullptr) {
        STRNCPY_UTF8(mix->label, current.layer->name);
        bNodeSocket *fac = socket_in(*mix, "Factor_Float");
        bNodeSocket *color1 = socket_in(*mix, "A_Color");
        bNodeSocket *color2 = socket_in(*mix, "B_Color");
        bNodeSocket *color_out = socket_out(*mix, "Result_Color");
        if (fac != nullptr && color1 != nullptr && color2 != nullptr && color_out != nullptr) {
          bke::node_add_link(tree, *previous.source_node, *previous.source, *mix, *color1);
          bke::node_add_link(tree, *current.source_node, *current.source, *mix, *color2);
          if (current.factor != nullptr && current.factor_node != nullptr) {
            bke::node_add_link(tree, *current.factor_node, *current.factor, *mix, *fac);
          }
          /* The straight content chain mirrors the Mix: lay the row's alpha over the alpha
           * below at Mix, keep the alpha below for any other blend. */
          bNode *mix_below_node = previous.content_alpha_node;
          bNodeSocket *mix_below = previous.content_alpha;
          previous = {current.layer,
                      mix,
                      color_out,
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      mix_below_node,
                      mix_below};
          if (track_content_alpha && current.content_alpha != nullptr &&
              current.content_alpha_node != nullptr)
          {
            const int mix_ramp = BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(
                BKE_paint_layers_channel_blend_effective(*current.layer, channel)));
            if (mix_below == nullptr) {
              bNode *mix_one = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
              mix_below = (mix_one != nullptr) ? socket_out(*mix_one, "Value") : nullptr;
              if (mix_below != nullptr && mix_below->default_value != nullptr) {
                static_cast<bNodeSocketValueFloat *>(mix_below->default_value)->value = 1.0f;
              }
              mix_below_node = mix_one;
            }
            if (mix_below != nullptr && mix_ramp == MA_RAMP_BLEND) {
              bNode *msub = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNode *mmul = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNode *madd = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNodeSocket *ms_a = (msub != nullptr) ? socket_in(*msub, "Value") : nullptr;
              bNodeSocket *ms_b = (msub != nullptr) ? socket_in(*msub, "Value_001") : nullptr;
              bNodeSocket *mm_a = (mmul != nullptr) ? socket_in(*mmul, "Value") : nullptr;
              bNodeSocket *mm_b = (mmul != nullptr) ? socket_in(*mmul, "Value_001") : nullptr;
              bNodeSocket *ma_a = (madd != nullptr) ? socket_in(*madd, "Value") : nullptr;
              bNodeSocket *ma_b = (madd != nullptr) ? socket_in(*madd, "Value_001") : nullptr;
              bNodeSocket *ms_out = (msub != nullptr) ? socket_out(*msub, "Value") : nullptr;
              bNodeSocket *mm_out = (mmul != nullptr) ? socket_out(*mmul, "Value") : nullptr;
              bNodeSocket *ma_out = (madd != nullptr) ? socket_out(*madd, "Value") : nullptr;
              if (ms_out != nullptr && mm_out != nullptr && ma_out != nullptr) {
                msub->custom1 = NODE_MATH_SUBTRACT;
                mmul->custom1 = NODE_MATH_MULTIPLY;
                madd->custom1 = NODE_MATH_ADD;
                bke::node_add_link(
                    tree, *current.content_alpha_node, *current.content_alpha, *msub, *ms_a);
                bke::node_add_link(tree, *mix_below_node, *mix_below, *msub, *ms_b);
                bke::node_add_link(tree, *msub, *ms_out, *mmul, *mm_a);
                if (current.factor != nullptr && current.factor_node != nullptr) {
                  bke::node_add_link(
                      tree, *current.factor_node, *current.factor, *mmul, *mm_b);
                }
                else if (mm_b != nullptr && mm_b->default_value != nullptr) {
                  static_cast<bNodeSocketValueFloat *>(mm_b->default_value)->value = 1.0f;
                }
                bke::node_add_link(tree, *mix_below_node, *mix_below, *madd, *ma_a);
                bke::node_add_link(tree, *mmul, *mm_out, *madd, *ma_b);
                mix_below_node = madd;
                mix_below = ma_out;
              }
            }
            previous.content_alpha_node = mix_below_node;
            previous.content_alpha = mix_below;
            content_alpha_node = mix_below_node;
            content_alpha = mix_below;
          }
        }
      }
    }
  }
  location_x += 180.0f;
  }
  return {previous, coverage_node, coverage, content_alpha_node, content_alpha};
}

RowResult PaintLayersChainBuilder::build_row(const MaterialPaintLayer *layer,
                                             const RowTarget &target,
                                             const bool substituted,
                                             Image *baked_color,
                                             const bool premul,
                                             LayerGroup *layer_group,
                                             const int channel)
{
  const Material &ma = outer_.ma_;
  const bool track_content_alpha =
      BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel(channel));
  /* A row a past rebuild left out of the graph contributes nothing. */
  if (row_is_removed(ma, *layer)) {
    return {};
  }
  /* A channel outside the material's set is not built, not even a Material row's live source. */
  if (paint_layer_channel_filtered(ma, *layer, channel)) {
    return {};
  }
  /* The aliases keep the row body unchanged; for a leaf target points at the row's own group,
   * where the value socket mirrors each root input the row uses. */
  bNodeTree &tree = *target.tree;
  bNode *group_input = target.group_input;
  /* The row's values are inputs of its own group; its Group Input node exposes them. */
  const float location_x = target.location_x;
  const float location_y = target.location_y;
  /* A Custom and a Material row alike have no generated subtree: a valid bake substitutes
   * above, and without one there is no channel record and the row drops out below. */

  /* Set while THIS row is built and it is itself a folder: its source is the sub-chain's
   * straight result, and its factor is multiplied by the sub-chain's coverage. These are
   * local to one #build_row call (not the enclosing #build_list's shared state): a
   * correction's own children -- and an isolating folder's own children -- recurse back into
   * #build_row for a different #layer, and a shared copy would carry that nested call's
   * folder bookkeeping right past its own return, into a completely unrelated row's coverage
   * multiply near this call's end (see the crash this fixed: a Stack correction's own row
   * inherited a grouped child's leftover folder-coverage node, in the child's tree, and linked
   * it into this row's tree). */
  bNode *folder_source_node = nullptr;
  bNodeSocket *folder_source = nullptr;
  bNode *folder_coverage_node = nullptr;
  bNodeSocket *folder_coverage = nullptr;
  bNode *folder_content_alpha_node = nullptr;
  bNodeSocket *folder_content_alpha = nullptr;
  /* The active Material row's live value for this channel, when it has one. A live constant
   * needs no sampler; a live map is the source's own texture. Both count as the row taking
   * part even without a channel record, so the early drop below must see them. */
  const RowMaterialSource row_source = outer_.resolve_row_material_source(
      *layer, channel, tree, substituted);
  const bool live_constant = row_source.live_constant;
  const bool live_map = row_source.live_map;
  const bNode *source_group_instance = row_source.source_group_instance;

  if (!substituted && BKE_paint_layers_is_folder(*layer)) {
    /* A folder: its contents are built in isolation -- pre-multiplied colour and coverage --
     * and then its own row lays the isolated result over what is below (design §5). */
    if (!build_folder_source(layer,
                             tree,
                             target,
                             channel,
                             track_content_alpha,
                             location_x,
                             location_y,
                             &folder_source_node,
                             &folder_source,
                             &folder_coverage_node,
                             &folder_coverage,
                             &folder_content_alpha_node,
                             &folder_content_alpha))
    {
      return {};
    }
  }
  else if (!substituted && !BKE_paint_layers_is_folder(*layer) &&
           !leaf_participates(ma, *layer, channel) && !live_constant && !live_map &&
           source_group_instance == nullptr)
  {
    if (!paint_layer_channel_present(ma, *layer, channel) &&
        layer->source == MA_PAINT_LAYER_SOURCE_NODE_GROUP &&
        custom_bake_missing_warn_once(ma, *layer))
    {
      fprintf(stderr,
              "Paint layers: Custom layer '%s' has no bake yet and is skipped until one "
              "lands\n",
              layer->name);
    }
    return {};
  }

  ChainLayer current;
  bNode *leaf_map_node = nullptr;
  if (!build_row_source(layer,
                        tree,
                        group_input,
                        channel,
                        substituted,
                        track_content_alpha,
                        location_x,
                        location_y,
                        row_source,
                        baked_color,
                        folder_source_node,
                        folder_source,
                        folder_content_alpha_node,
                        folder_content_alpha,
                        current,
                        leaf_map_node))
  {
    return {};
  }

/* The grey of \a image (the mean of RGB) as a new node chain, the way the CPU reads a mask
 * item and a coverage map (#PaintMaterialCompositeImageLayer::mask_reads_grey,
 * ::coverage_image): masks are painted black and white, and a brush writes colour, not alpha. */

/* A Material/Node Group correction's own coverage: the same Baked/Hybrid/SourceGroup
 * precedence #resolve_row_material_source uses for content, but always read on the Alpha
 * channel and ending at the wrapper's dedicated COVERAGE output (SourceGroup) or the
 * correction's own bake coverage (Baked) -- mirrors `layer_factor` below (~2276-2356)
 * exactly, with \a row standing in for `*layer`. One helper for a content Effect (its own
 * coverage multiplier) and a Mask Item (its coverage when `mask_channel` is not Alpha), so
 * the two cannot disagree about what "the source's coverage" means. */

/* The factor base the mask stack builds on: one, times -- for a Material layer -- its
 * source's coverage (what the source's own transparency baked into), so a mask on a
 * transparent source limits it further and never re-bakes it. */
bNode *layer_factor_node = nullptr;
bNodeSocket *layer_factor_socket = nullptr;
resolve_row_factor(layer,
                   tree,
                   location_x,
                   location_y,
                   substituted,
                   row_source,
                   layer_factor_node,
                   layer_factor_socket);

/* The row's own content coverage, kept apart from the mask: an unpainted texel of a fresh map
 * is transparent black and must show the rows below, and the content corrections raise this
 * coverage. The mask multiplies it in afterwards, so a mask always clips a correction.
 *
 * A Material row has no such coverage of its own: the Principled's Base Color reads RGB only,
 * so the channel map's alpha (a real bake is opaque) must not be folded into the factor -- the
 * material's transparency already arrived as `layer_factor` from the Alpha input. */
build_row_factor_chain(layer,
                       target,
                       tree,
                       group_input,
                       channel,
                       substituted,
                       track_content_alpha,
                       location_x,
                       location_y,
                       leaf_map_node,
                       folder_coverage_node,
                       folder_coverage,
                       current,
                       layer_factor_node,
                       layer_factor_socket);

  RowResult result;

  if (!build_grouped_row_result(layer,
                                tree,
                                group_input,
                                layer_group,
                                channel,
                                premul,
                                location_x,
                                location_y,
                                current,
                                result))
  {
    return {};
  }

  result.valid = true;
  result.current = current;
  result.folder_coverage_node = folder_coverage_node;
  result.folder_coverage = folder_coverage;
  result.folder_content_alpha_node = folder_content_alpha_node;
  result.folder_content_alpha = folder_content_alpha;
  return result;
}

bool PaintLayersChainBuilder::build_folder_source(const MaterialPaintLayer *layer,
                                                  bNodeTree &tree,
                                                  const RowTarget &target,
                                                  const int channel,
                                                  const bool track_content_alpha,
                                                  const float location_x,
                                                  const float location_y,
                                                  bNode **r_source_node,
                                                  bNodeSocket **r_source,
                                                  bNode **r_coverage_node,
                                                  bNodeSocket **r_coverage,
                                                  bNode **r_content_alpha_node,
                                                  bNodeSocket **r_content_alpha)
{
  bNode *p_zero = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
  bNodeSocket *p_zero_out = (p_zero != nullptr) ? socket_out(*p_zero, "Color") : nullptr;
  if (p_zero_out == nullptr) {
    return false;
  }
  if (p_zero_out->default_value != nullptr) {
    copy_v4_fl(
        static_cast<bNodeSocketValueRGBA *>(p_zero_out->default_value)->value, 0.0f);
  }
  ChainLayer sub_previous;
  sub_previous.source_node = p_zero;
  sub_previous.source = p_zero_out;
  ChainResult sub = build_list(layer->children, sub_previous, true, target, channel);
  if (sub.chain.source == nullptr || sub.coverage == nullptr) {
    return false;
  }
  /* S_folder = P / a; the Vector Math divide is per channel and safe (0 on zero), so an
   * empty folder answers zero and covers nothing. */
  bNode *divide = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
  bNodeSocket *div_a = (divide != nullptr) ? socket_in(*divide, "Vector") : nullptr;
  bNodeSocket *div_b = (divide != nullptr) ? socket_in(*divide, "Vector_001") : nullptr;
  bNodeSocket *div_out = (divide != nullptr) ? socket_out(*divide, "Vector") : nullptr;
  if (div_out == nullptr) {
    return false;
  }
  divide->custom1 = NODE_VECTOR_MATH_DIVIDE;
  divide->location[0] = location_x;
  divide->location[1] = location_y - 480.0f;
  bke::node_add_link(tree, *sub.chain.source_node, *sub.chain.source, *divide, *div_a);
  bke::node_add_link(tree, *sub.coverage_node, *sub.coverage, *divide, *div_b);
  *r_source_node = divide;
  *r_source = div_out;
  *r_coverage_node = sub.coverage_node;
  *r_coverage = sub.coverage;
  /* The content alpha is accumulated beside the premultiplied colour, so it is straightened
   * the same way: S_alpha = P_alpha / a. A null accumulation means no Paint leaf tracked an
   * alpha here, so the chain stays null and no Content Alpha socket is built. */
  *r_content_alpha_node = nullptr;
  *r_content_alpha = nullptr;
  if (track_content_alpha && sub.content_alpha != nullptr) {
    bNode *alpha_divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
    bNodeSocket *ad_a = (alpha_divide != nullptr) ? socket_in(*alpha_divide, "Value") :
                                                    nullptr;
    bNodeSocket *ad_b = (alpha_divide != nullptr) ? socket_in(*alpha_divide, "Value_001") :
                                                    nullptr;
    bNodeSocket *ad_out = (alpha_divide != nullptr) ? socket_out(*alpha_divide, "Value") :
                                                      nullptr;
    if (ad_out != nullptr) {
      alpha_divide->custom1 = NODE_MATH_DIVIDE;
      alpha_divide->location[0] = location_x;
      alpha_divide->location[1] = location_y - 400.0f;
      bke::node_add_link(
          tree, *sub.content_alpha_node, *sub.content_alpha, *alpha_divide, *ad_a);
      bke::node_add_link(tree, *sub.coverage_node, *sub.coverage, *alpha_divide, *ad_b);
      *r_content_alpha_node = alpha_divide;
      *r_content_alpha = ad_out;
    }
  }
  return true;
}

bool PaintLayersChainBuilder::build_row_source(const MaterialPaintLayer *layer,
                                               bNodeTree &tree,
                                               bNode *group_input,
                                               const int channel,
                                               const bool substituted,
                                               const bool track_content_alpha,
                                               const float location_x,
                                               const float location_y,
                                               const RowMaterialSource &row_source,
                                               Image *baked_color,
                                               bNode *folder_source_node,
                                               bNodeSocket *folder_source,
                                               bNode *folder_content_alpha_node,
                                               bNodeSocket *folder_content_alpha,
                                               ChainLayer &r_current,
                                               bNode *&r_leaf_map_node)
{
  const Material &ma = outer_.ma_;
  const PaintLayersRegenCache *const cache = outer_.cache_;
  auto &opacity_inputs = outer_.opacity_inputs_;
  auto &fill_inputs = outer_.fill_inputs_;
  auto &live_constant_inputs = outer_.live_constant_inputs_;
  const bool live_constant = row_source.live_constant;
  const bool live_map = row_source.live_map;
  Image *live_map_image = row_source.live_map_image;
  const ImageUser *live_map_iuser = row_source.live_map_iuser;
  bNode *source_group_instance = row_source.source_group_instance;
  bNodeSocket *source_group_socket = row_source.source_group_socket;

  ChainLayer &current = r_current;
  bNode *&leaf_map_node = r_leaf_map_node;
  current.layer = layer;
  if (Map<int, bNodeTreeInterfaceSocket *> *opacity_by_channel =
          opacity_inputs.lookup_ptr(layer))
  {
    if (bNodeTreeInterfaceSocket **opacity_iface = opacity_by_channel->lookup_ptr(channel)) {
      current.opacity_node = group_input;
      current.opacity = group_input_socket(group_input, **opacity_iface);
    }
  }

  /* The row's own channel map, when it has one: its alpha is the row's per-pixel coverage, the
   * same read #composite_image_layers_build sets up with #color_alpha_coverage. */
  leaf_map_node = nullptr;
  if (substituted) {
    /* Variant C: the baked color Alpha is the live map Alpha the row had (or the folder subtree
     * content), so the factor chain below multiplies `common x content` exactly like the live
     * `mask x content` chain. A constant leaf had no live map alpha, so it keeps covering by
     * common alone. Material and mesh-map rows never reach this branch. */
    const bool use_baked_content =
        !ELEM(layer->source, MA_PAINT_LAYER_SOURCE_MATERIAL, MA_PAINT_LAYER_SOURCE_MESH_MAP) &&
        (BKE_paint_layers_is_folder(*layer) ||
         paint_layer_channel_image(ma, *layer, channel) != nullptr);
    bNode *baked_content_node = nullptr;
    if (!build_substituted_source(layer,
                                  tree,
                                  track_content_alpha,
                                  location_x,
                                  location_y,
                                  baked_color,
                                  current,
                                  channel,
                                  use_baked_content,
                                  &baked_content_node))
    {
      return false;
    }
    leaf_map_node = baked_content_node;
    /* The baked factor already holds mask x opacity, so the row's own opacity input is not used
     * again; only visibility is multiplied in, from its own 0/1 input. */
    bNodeSocket *enabled_out = nullptr;
    if (bNodeTreeInterfaceSocket *const *enabled_iface = outer_.enabled_inputs_.lookup_ptr(layer)) {
      enabled_out = group_input_socket(group_input, **enabled_iface);
    }
    if (enabled_out != nullptr && current.opacity != nullptr) {
      bNode *visible = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
      bNodeSocket *visible_a = (visible != nullptr) ? socket_in(*visible, "Value") : nullptr;
      bNodeSocket *visible_b = (visible != nullptr) ? socket_in(*visible, "Value_001") : nullptr;
      if (visible_a == nullptr || visible_b == nullptr) {
        return false;
      }
      visible->custom1 = NODE_MATH_MULTIPLY;
      visible->location[0] = location_x + 90.0f;
      visible->location[1] = location_y - 160.0f;
      bke::node_add_link(tree, *current.opacity_node, *current.opacity, *visible, *visible_a);
      bke::node_add_link(tree, *group_input, *enabled_out, *visible, *visible_b);
      current.opacity_node = visible;
      current.opacity = socket_out(*visible, "Value");
    }
    printf("[PL-DIAG] substituted enabled row='%s' channel=%d enabled_input=%d\n",
           layer->name,
           channel,
           int(enabled_out != nullptr));
  }
  else {
    /* A constant answers first: it needs no sampler, so a channel the resolver calls Constant
     * never falls through to an Image result. */
    Image *image = (live_constant || live_map || source_group_instance != nullptr) ?
                       nullptr :
                       paint_layer_channel_image(ma, *layer, channel);
    if (live_constant) {
      /* The active Material row shows its source's live constant rather than its baked map.
       * The value lives in another material, so it is not topology (ТЗ-26): it is read from
       * this row's own group input, filled by #create_value_inputs and kept current by
       * #values_sync_socket, exactly like a row's own Fill constant. */
      bNodeTreeInterfaceSocket *live_constant_iface = nullptr;
      if (Map<int, bNodeTreeInterfaceSocket *> *live_constant_by_channel =
              live_constant_inputs.lookup_ptr(layer))
      {
        if (bNodeTreeInterfaceSocket **found = live_constant_by_channel->lookup_ptr(channel)) {
          live_constant_iface = *found;
        }
      }
      bNodeSocket *constant_out = (live_constant_iface != nullptr) ?
                                      group_input_socket(group_input, *live_constant_iface) :
                                      nullptr;
      if (constant_out == nullptr) {
        return false;
      }
      current.source_node = group_input;
      current.source = constant_out;
      /* A Material row's transparency is the Alpha input, never the channel's own value: the
       * Principled ignores Base Color's RGBA alpha. It tracks no content alpha of its own --
       * the material alpha already fed `layer_factor` -- so the Result alpha keeps the chain's
       * blended value, exactly as the CPU computes it. */
    }
    else if (live_map) {
      /* The row shows the source's own texture. Its sampling settings travel with it; no Divide
       * is built here, exactly like the row's own map branch below -- the Image Texture node
       * handles a non-data texture's un-premultiply itself. */
      bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
      if (map == nullptr) {
        return false;
      }
      map->id = &live_map_image->id;
      id_us_plus(&live_map_image->id);
      map->location[0] = location_x;
      map->location[1] = location_y;
      if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
        if (live_map_iuser != nullptr) {
          dst->iuser = *live_map_iuser;
        }
        MaterialSourceResolve resolve_local;
        const MaterialSourceResolve &resolve = PaintLayersRegenCache::resolve_get(
            layer->material, cache, resolve_local);
        const bNode *src_node = resolve.images[channel].node;
        if (const NodeTexImage *src_storage =
                (src_node != nullptr) ? static_cast<const NodeTexImage *>(src_node->storage) :
                                        nullptr)
        {
          dst->interpolation = src_storage->interpolation;
          dst->extension = src_storage->extension;
          dst->projection = src_storage->projection;
        }
      }
      current.source_node = map;
      current.source = socket_out(*map, "Color");
      leaf_map_node = map;
      /* A Material row's transparency is the source's Alpha input, not the channel map's own
       * alpha (the Principled's Base Color reads RGB only). It tracks no content alpha of its
       * own: the material alpha already fed `layer_factor`, and leaving the chain null keeps
       * the Result alpha the chain's blended value, exactly as the CPU computes it. */
    }
    else if (source_group_instance != nullptr) {
      /* The whole source graph goes through the wrapper's COLOR:<CHANNEL> output. No map, so
       * content coverage for this row comes from the wrapper's COVERAGE output below. */
      if (source_group_socket == nullptr) {
        PL_DEBUG_PRINTF(
            "paint layers: row '%s' channel %d: wrapper has no COLOR output, row dropped\n",
            layer->name,
            channel);
        return false;
      }
      current.source_node = source_group_instance;
      current.source = source_group_socket;
    }
    else if (folder_source != nullptr) {
      /* The folder's own row: its source is the isolated sub-chain, not a map. */
      current.source_node = folder_source_node;
      current.source = folder_source;
      current.content_alpha_node = folder_content_alpha_node;
      current.content_alpha = folder_content_alpha;
    }
    else if (image != nullptr) {
      bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
      if (map == nullptr) {
        return false;
      }
      map->id = &image->id;
      id_us_plus(&image->id);
      map->location[0] = location_x;
      map->location[1] = location_y;
      /* A MESH_MAP atlas is sampled with Extend (clamp to the edge), the mode the CPU's own
       * bilinear resample uses, so a differently sized atlas agrees at its borders. A painted
       * map keeps its Repeat default. Read through the shared resolver so the two sides cannot
       * disagree about whether the row is a mesh map at all. */
      const bool mesh_map = layer->source == MA_PAINT_LAYER_SOURCE_MESH_MAP &&
                            paint_layer_mesh_map_image(ma, *layer) == image;
      if (mesh_map) {
        if (NodeTexImage *storage = static_cast<NodeTexImage *>(map->storage)) {
          storage->extension = SHD_IMAGE_EXTENSION_EXTEND;
        }
      }
      current.source_node = map;
      current.source = socket_out(*map, "Color");
      /* A scalar atlas (AO, Curvature, Edge) stores its value in R; spread it across RGB so a
       * colour channel reads it as grey and the CPU reads the same. An RGB atlas (Normal, IDs)
       * is wired as it is. */
      if (mesh_map && paint_layer_mesh_map_is_scalar(layer->mesh_map_type)) {
        bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
        bNode *combine = bke::node_add_node(nullptr, tree, "ShaderNodeCombineXYZ"_ustr);
        if (separate != nullptr && combine != nullptr) {
          bNodeSocket *sep_vector = socket_in(*separate, "Vector");
          bNodeSocket *sep_x = socket_out(*separate, "X");
          bNodeSocket *combine_x = socket_in(*combine, "X");
          bNodeSocket *combine_y = socket_in(*combine, "Y");
          bNodeSocket *combine_z = socket_in(*combine, "Z");
          if (sep_vector != nullptr && sep_x != nullptr && combine_x != nullptr &&
              combine_y != nullptr && combine_z != nullptr)
          {
            separate->location[0] = location_x + 80.0f;
            separate->location[1] = location_y;
            combine->location[0] = location_x + 160.0f;
            combine->location[1] = location_y;
            bke::node_add_link(tree, *map, *current.source, *separate, *sep_vector);
            bke::node_add_link(tree, *separate, *sep_x, *combine, *combine_x);
            bke::node_add_link(tree, *separate, *sep_x, *combine, *combine_y);
            bke::node_add_link(tree, *separate, *sep_x, *combine, *combine_z);
            current.source_node = combine;
            current.source = socket_out(*combine, "Vector");
          }
        }
      }
      leaf_map_node = map;
      /* A Paint or Fill map carries its content alpha in the Image Texture Alpha output; it
       * starts the content-alpha chain here rather than being read back out of the Color's own
       * alpha. Custom stays outside since its bake substitutes above instead of reaching this
       * branch.
       *
       * A Material row never takes its content alpha from the channel map: the Principled's
       * transparency is the Alpha input, and a real bake map is opaque. It tracks no content
       * alpha at all, so the Result alpha stays the chain's blended value like the CPU's. */
      if (track_content_alpha &&
          (BKE_paint_layers_role(*layer) == PaintLayerRole::Layer &&
          ELEM(layer->source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)))
      {
        current.content_alpha_node = map;
        current.content_alpha = socket_out(*map, "Alpha");
      }
    }
    else {
      const MaterialPaintLayerChannel *entry = paint_layer_channel_find(*layer, channel);
      if (bNodeTreeInterfaceSocket **fill_iface = fill_inputs.lookup_ptr(entry)) {
        current.opacity_node = group_input;
        current.source_node = group_input;
        current.source = group_input_socket(group_input, **fill_iface);
      }
      /* A Paint or Fill constant's alpha is the constant colour's `.a`, the same number the RGB
       * Fill input carries; it is frozen into a Value so the content-alpha chain has a scalar
       * leaf. Custom and Material leaves (F2-C3/C4) supply no such constant. */
      if (track_content_alpha &&
          (BKE_paint_layers_role(*layer) == PaintLayerRole::Layer &&
          ELEM(layer->source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)) &&
          current.source != nullptr)
      {
        float constant[4];
        paint_layer_channel_constant(*layer, channel, constant);
        bNode *alpha_value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
        bNodeSocket *alpha_out = (alpha_value != nullptr) ? socket_out(*alpha_value, "Value") :
                                                            nullptr;
        if (alpha_value != nullptr && alpha_out != nullptr &&
            alpha_out->default_value != nullptr)
        {
          alpha_value->location[0] = location_x - 90.0f;
          alpha_value->location[1] = location_y - 40.0f;
          static_cast<bNodeSocketValueFloat *>(alpha_out->default_value)->value = constant[3];
          current.content_alpha_node = alpha_value;
          current.content_alpha = alpha_out;
        }
      }
    }
  }
  if (current.source == nullptr) {
    return false;
  }
  return true;
}

bool PaintLayersChainBuilder::build_substituted_source(const MaterialPaintLayer *layer,
                                                       bNodeTree &tree,
                                                       const bool track_content_alpha,
                                                       const float location_x,
                                                       const float location_y,
                                                       Image *baked_color,
                                                       ChainLayer &r_current,
                                                       const int channel,
                                                       const bool use_baked_content,
                                                       bNode **r_content_node)
{
  ChainLayer &current = r_current;
  bNode *baked_color_node = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  bNode *baked_coverage_node = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  if (baked_color_node == nullptr || baked_coverage_node == nullptr) {
    return false;
  }
  baked_color_node->id = &baked_color->id;
  id_us_plus(&baked_color->id);
  baked_color_node->location[0] = location_x;
  baked_color_node->location[1] = location_y;
  /* A substituted row's bake stores content alpha in the same Image Texture Alpha output as a
   * live Paint/Fill map (both write IMA_ALPHA_STRAIGHT, #bake_image_ensure), so it starts the
   * content-alpha chain here exactly as the live leaf does at its own Image Texture node. Not
   * gated by kind: Custom has no live path and only reaches the generator through this branch,
   * and Material never reaches it at all (its bake substitutes a whole subtree, not a row). */
  if (track_content_alpha) {
    current.content_alpha_node = baked_color_node;
    current.content_alpha = socket_out(*baked_color_node, "Alpha");
  }
  baked_coverage_node->id = &layer->bake->coverage->id;
  id_us_plus(&layer->bake->coverage->id);
  baked_coverage_node->location[0] = location_x - 90.0f;
  baked_coverage_node->location[1] = location_y - 160.0f;
  current.source_node = baked_color_node;
  current.source = socket_out(*baked_color_node, "Color");
  /* The coverage map stores the scalar in grey RGB (alpha = 1, mask-correction convention);
   * the factor is the mean of the three channels, exactly as the CPU reads it. */
  bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
  bNode *add_xy = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  bNode *add_z = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  bNode *divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (separate != nullptr && add_xy != nullptr && add_z != nullptr && divide != nullptr) {
    add_xy->custom1 = NODE_MATH_ADD;
    add_z->custom1 = NODE_MATH_ADD;
    divide->custom1 = NODE_MATH_DIVIDE;
    bNodeSocket *sep_vector = socket_in(*separate, "Vector");
    bNodeSocket *sep_x = socket_out(*separate, "X");
    bNodeSocket *sep_y = socket_out(*separate, "Y");
    bNodeSocket *sep_z = socket_out(*separate, "Z");
    bNodeSocket *xy_a = socket_in(*add_xy, "Value");
    bNodeSocket *xy_b = socket_in(*add_xy, "Value_001");
    bNodeSocket *z_a = socket_in(*add_z, "Value");
    bNodeSocket *z_b = socket_in(*add_z, "Value_001");
    bNodeSocket *d_a = socket_in(*divide, "Value");
    bNodeSocket *d_b = socket_in(*divide, "Value_001");
    if (sep_vector == nullptr || sep_x == nullptr || sep_y == nullptr || sep_z == nullptr ||
        xy_a == nullptr || xy_b == nullptr || z_a == nullptr || z_b == nullptr ||
        d_a == nullptr || d_b == nullptr)
    {
      return false;
    }
    bke::node_add_link(tree,
                       *baked_coverage_node,
                       *socket_out(*baked_coverage_node, "Color"),
                       *separate,
                       *sep_vector);
    bke::node_add_link(tree, *separate, *sep_x, *add_xy, *xy_a);
    bke::node_add_link(tree, *separate, *sep_y, *add_xy, *xy_b);
    bke::node_add_link(tree, *add_xy, *socket_out(*add_xy, "Value"), *add_z, *z_a);
    bke::node_add_link(tree, *separate, *sep_z, *add_z, *z_b);
    bke::node_add_link(tree, *add_z, *socket_out(*add_z, "Value"), *divide, *d_a);
    if (d_b->default_value != nullptr) {
      static_cast<bNodeSocketValueFloat *>(d_b->default_value)->value = 3.0f;
    }
    current.opacity_node = divide;
    current.opacity = socket_out(*divide, "Value");
  }
  /* Variant C: the baked coverage is the channel-independent `common = mask x opacity`
   * (mean of grey RGB below), and per-channel content comes from the baked color Alpha exactly
   * like a live leaf map. Hand the baked color node out when the live row had content of its own
   * (a map or a folder subtree); a constant leaf covers by common alone, as its live factor does. */
  if (r_content_node != nullptr) {
    *r_content_node = use_baked_content ? baked_color_node : nullptr;
  }
  /* [PL-DIAG] What a substituted row reads from its bake, for the correction-visibility report.
   * Prints only (the pixel read goes through the same acquire the CPU composite and the GPU upload
   * use). Compare `alpha_px` with the heavy-bake line: a value near zero here, with the bake line
   * large, means the map lost its content after the commit. */
  {
    auto diag_image = [](const char *what, Image *image) {
      if (image == nullptr) {
        printf("[PL-DIAG]   %s: null\n", what);
        return;
      }
      /* The full-pixel scan of two large maps ran on the main thread at every regen (about 35 ms
       * of `groups_scratch_build`). It stays available behind an environment flag. */
      static const bool scan = getenv("PL_DIAG_SCAN") != nullptr;
      if (!scan) {
        printf("[PL-DIAG]   %s: '%s' %p us=%d cs='%s' alpha_mode=%d flag=%d (set PL_DIAG_SCAN=1 "
               "for the pixel scan)\n",
               what,
               image->id.name + 2,
               static_cast<void *>(image),
               image->id.us,
               image->colorspace_settings.name,
               int(image->alpha_mode),
               int(image->flag));
        return;
      }
      void *lock = nullptr;
      ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
      int64_t alpha_px = 0, rgb_px = 0;
      int dirty = -1, width = 0, height = 0;
      if (ibuf != nullptr && ibuf->byte_data() != nullptr) {
        width = ibuf->x;
        height = ibuf->y;
        dirty = (ibuf->userflags & IB_BITMAPDIRTY) ? 1 : 0;
        const uchar *px = ibuf->byte_data();
        for (int64_t i = 0; i < int64_t(width) * height; i++) {
          if (px[i * 4 + 3] > 0) {
            alpha_px++;
            rgb_px += (px[i * 4] | px[i * 4 + 1] | px[i * 4 + 2]) != 0 ? 1 : 0;
          }
        }
      }
      printf("[PL-DIAG]   %s: '%s' %p us=%d cs='%s' alpha_mode=%d flag=%d size=%dx%d ibuf_dirty=%d "
             "alpha_px=%lld rgb_nonzero_where_alpha=%lld\n",
             what,
             image->id.name + 2,
             static_cast<void *>(image),
             image->id.us,
             image->colorspace_settings.name,
             int(image->alpha_mode),
             int(image->flag),
             width,
             height,
             dirty,
             static_cast<long long>(alpha_px),
             static_cast<long long>(rgb_px));
      BKE_image_release_ibuf(image, ibuf, lock);
    };
    printf("[PL-DIAG] substituted source row='%s' content_alpha_from=baked_color.Alpha(%d) "
           "factor=mean(coverage.RGB)(%d) live_correction_nodes=none\n",
           layer->name,
           int(current.content_alpha != nullptr),
           int(current.opacity != nullptr));
    printf("[PL-DIAG] substituted factor row='%s' channel=%d mode=%s\n",
           layer->name,
           channel,
           use_baked_content ? "common x baked_alpha" : "common");
    diag_image("color", baked_color);
    diag_image("coverage", layer->bake->coverage);
  }
  return true;
}

void PaintLayersChainBuilder::resolve_row_factor(const MaterialPaintLayer *layer,
                                                 bNodeTree &tree,
                                                 const float location_x,
                                                 const float location_y,
                                                 const bool substituted,
                                                 const RowMaterialSource &row_source,
                                                 bNode *&r_factor_node,
                                                 bNodeSocket *&r_factor_socket)
{
  const Material &ma = outer_.ma_;
  const PaintLayersRegenCache *const cache = outer_.cache_;
  bNode *&layer_factor_node = r_factor_node;
  bNodeSocket *&layer_factor_socket = r_factor_socket;
  bNode *source_group_instance = row_source.source_group_instance;
  bNodeTree *source_group_tree = row_source.source_group_tree;
  float live_alpha[4];
  Image *live_alpha_image = nullptr;
  const ImageUser *live_alpha_iuser = nullptr;
  if (!substituted && BKE_paint_layers_material_live_constant(
                          ma, *layer, PAINT_MATERIAL_CHANNEL_ALPHA, live_alpha, cache))
  {
    /* The source's alpha is a constant too, so the coverage stays live with it. When the
     * source's alpha is not constant while its other channels are, the coverage keeps its last
     * bake -- a bounded divergence: only channels that can be shown live are. */
    bNode *value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
    bNodeSocket *value_out = (value != nullptr) ? socket_out(*value, "Value") : nullptr;
    if (value != nullptr && value_out != nullptr && value_out->default_value != nullptr) {
      value->location[0] = location_x - 90.0f;
      value->location[1] = location_y - 320.0f;
      static_cast<bNodeSocketValueFloat *>(value_out->default_value)->value = live_alpha[0];
      layer_factor_node = value;
      layer_factor_socket = value_out;
    }
  }
  else if (!substituted &&
           BKE_paint_layers_material_live_image(ma,
                                                *layer,
                                                PAINT_MATERIAL_CHANNEL_ALPHA,
                                                &live_alpha_image,
                                                &live_alpha_iuser,
                                                cache))
  {
    /* The source's alpha is a live texture: the factor is that map's Alpha output, the same
     * output the CPU reads as its coverage. */
    bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
    bNodeSocket *map_alpha = (map != nullptr) ? socket_out(*map, "Alpha") : nullptr;
    if (map != nullptr && map_alpha != nullptr) {
      map->id = &live_alpha_image->id;
      id_us_plus(&live_alpha_image->id);
      map->location[0] = location_x - 90.0f;
      map->location[1] = location_y - 320.0f;
      if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
        if (live_alpha_iuser != nullptr) {
          dst->iuser = *live_alpha_iuser;
        }
        MaterialSourceResolve resolve_local;
        const MaterialSourceResolve &resolve = PaintLayersRegenCache::resolve_get(
            layer->material, cache, resolve_local);
        const bNode *src_node = resolve.images[PAINT_MATERIAL_CHANNEL_ALPHA].node;
        if (const NodeTexImage *src_storage =
                (src_node != nullptr) ? static_cast<const NodeTexImage *>(src_node->storage) :
                                        nullptr)
        {
          dst->interpolation = src_storage->interpolation;
          dst->extension = src_storage->extension;
          dst->projection = src_storage->projection;
        }
      }
      layer_factor_node = map;
      layer_factor_socket = map_alpha;
    }
  }
  else if (!substituted && source_group_instance != nullptr && source_group_tree != nullptr) {
    /* The wrapper's own COVERAGE output is the row's factor. Without one it behaves like a
     * source with no live alpha: the baked coverage stands in. */
    bNodeSocket *coverage_out = source_group_output(
        *source_group_tree, *source_group_instance, PAINT_MATERIAL_CHANNEL_ALPHA, true);
    if (coverage_out != nullptr) {
      layer_factor_node = source_group_instance;
      layer_factor_socket = coverage_out;
    }
    else if (layer->source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer->bake != nullptr &&
             layer->bake->coverage != nullptr)
    {
      std::tie(layer_factor_node, layer_factor_socket) = build_grey_of_map(
          tree, location_x, location_y, *layer->bake->coverage, -320.0f);
    }
  }
  else if (!substituted && layer->source == MA_PAINT_LAYER_SOURCE_MATERIAL &&
           layer->bake != nullptr && layer->bake->coverage != nullptr)
  {
    std::tie(layer_factor_node, layer_factor_socket) = build_grey_of_map(
        tree, location_x, location_y, *layer->bake->coverage, -320.0f);
  }
}

void PaintLayersChainBuilder::build_row_factor_chain(const MaterialPaintLayer *layer,
                                                     const RowTarget &target,
                                                     bNodeTree &tree,
                                                     bNode *group_input,
                                                     const int channel,
                                                     const bool substituted,
                                                     const bool track_content_alpha,
                                                     const float location_x,
                                                     const float location_y,
                                                     bNode *leaf_map_node,
                                                     bNode *&folder_coverage_node,
                                                     bNodeSocket *&folder_coverage,
                                                     ChainLayer &current,
                                                     bNode *&r_factor_node,
                                                     bNodeSocket *&r_factor_socket)
{
  bNode *&layer_factor_node = r_factor_node;
  bNodeSocket *&layer_factor_socket = r_factor_socket;
  bNode *content_cov_node = nullptr;
  bNodeSocket *content_cov = nullptr;
  if (leaf_map_node != nullptr &&
      !ELEM(layer->source, MA_PAINT_LAYER_SOURCE_MATERIAL, MA_PAINT_LAYER_SOURCE_MESH_MAP))
  {
    content_cov = socket_out(*leaf_map_node, "Alpha");
    content_cov_node = (content_cov != nullptr) ? leaf_map_node : nullptr;
  }

  /* Content corrections adjust the colour the row paints with, before its own blend. Only the
   * exact subset the CPU composites: a Paint correction backed by a map, or a Fill correction
   * carrying a constant. On the Normal channel a content Paint correction rides the same
   * Normal Combine the row does, while a content Fill correction (a constant normal) is left
   * out here too, so the two never disagree about which rows contributed. */
  build_content_correction(layer,
                           target,
                           substituted,
                           channel,
                           tree,
                           group_input,
                           location_x,
                           location_y,
                           track_content_alpha,
                           current,
                           content_cov_node,
                           content_cov,
                           folder_coverage_node,
                           folder_coverage);

  /* The layer factor is the mask times the content coverage; either alone when the other is
   * absent. */
  if (content_cov != nullptr && content_cov_node != nullptr) {
    if (layer_factor_socket == nullptr) {
      layer_factor_node = content_cov_node;
      layer_factor_socket = content_cov;
    }
    else {
      bNode *product = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
      bNodeSocket *p_a = (product != nullptr) ? socket_in(*product, "Value") : nullptr;
      bNodeSocket *p_b = (product != nullptr) ? socket_in(*product, "Value_001") : nullptr;
      bNodeSocket *p_out = (product != nullptr) ? socket_out(*product, "Value") : nullptr;
      if (p_a != nullptr && p_b != nullptr && p_out != nullptr) {
        product->custom1 = NODE_MATH_MULTIPLY;
        product->location[0] = location_x + 350.0f;
        product->location[1] = location_y - 420.0f;
        bke::node_add_link(tree, *layer_factor_node, *layer_factor_socket, *product, *p_a);
        bke::node_add_link(tree, *content_cov_node, *content_cov, *product, *p_b);
        layer_factor_node = product;
        layer_factor_socket = p_out;
      }
    }
  }

  /* Mask items are a coverage stack over the row factor: each item lays its own coverage
   * `F = mix(F_below, blend(F_below, C), A * op)` by its own blend mode (`#build_mask_item`), where
   * `C` is the mean of the map's raw Color (a Non-Color map reads un-premultiplied, so this is the
   * pre-multiplied color) and `A` its Alpha, and `op` the row's own opacity. Mix is the plain over
   * the chain always used. Later list entries lay over earlier ones; the opacity is row-level, not
   * per channel. */
  build_mask_item(layer,
                  channel,
                  tree,
                  group_input,
                  location_x,
                  location_y,
                  target,
                  current,
                  layer_factor_node,
                  layer_factor_socket);

  /* The factor is the correction/mask chain times the row's own opacity, or the opacity alone
   * when there is neither a mask map nor a correction. */
  if (layer_factor_socket != nullptr && current.opacity != nullptr &&
      current.opacity_node != nullptr)
  {
    bNode *opacity_multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
    if (opacity_multiply != nullptr) {
      opacity_multiply->custom1 = NODE_MATH_MULTIPLY;
      opacity_multiply->location[0] = location_x + 200.0f;
      opacity_multiply->location[1] = location_y - 320.0f;
      bNodeSocket *value_a = socket_in(*opacity_multiply, "Value");
      bNodeSocket *value_b = socket_in(*opacity_multiply, "Value_001");
      bNodeSocket *value_out = socket_out(*opacity_multiply, "Value");
      if (value_a != nullptr && value_b != nullptr && value_out != nullptr) {
        bke::node_add_link(
            tree, *current.opacity_node, *current.opacity, *opacity_multiply, *value_a);
        bke::node_add_link(
            tree, *layer_factor_node, *layer_factor_socket, *opacity_multiply, *value_b);
        current.factor_node = opacity_multiply;
        current.factor = value_out;
      }
    }
  }
  else {
    current.factor_node = current.opacity_node;
    current.factor = current.opacity;
  }

  /* A folder's row blends by its own factor times the coverage its contents accumulated. */
  if (folder_coverage_node != nullptr && folder_coverage != nullptr) {
    bNode *coverage_multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
    if (coverage_multiply != nullptr) {
      coverage_multiply->custom1 = NODE_MATH_MULTIPLY;
      coverage_multiply->location[0] = location_x + 150.0f;
      coverage_multiply->location[1] = location_y - 400.0f;
      bNodeSocket *cv_a = socket_in(*coverage_multiply, "Value");
      bNodeSocket *cv_b = socket_in(*coverage_multiply, "Value_001");
      bNodeSocket *cv_out = socket_out(*coverage_multiply, "Value");
      if (cv_a != nullptr && cv_b != nullptr && cv_out != nullptr) {
        if (current.factor != nullptr && current.factor_node != nullptr) {
          bke::node_add_link(
              tree, *current.factor_node, *current.factor, *coverage_multiply, *cv_a);
        }
        else if (cv_a->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(cv_a->default_value)->value = 1.0f;
        }
        bke::node_add_link(
            tree, *folder_coverage_node, *folder_coverage, *coverage_multiply, *cv_b);
        current.factor_node = coverage_multiply;
        current.factor = cv_out;
      }
    }
  }
}

bool PaintLayersChainBuilder::build_grouped_row_result(const MaterialPaintLayer *layer,
                                                       bNodeTree &tree,
                                                       bNode *group_input,
                                                       LayerGroup *layer_group,
                                                       const int channel,
                                                       const bool premul,
                                                       const float location_x,
                                                       const float location_y,
                                                       ChainLayer &current,
                                                       RowResult &r_result)
{
  const PaintLayersBuildContext &ctx = outer_.ctx_;
  /* A leaf in its own group: expose the row's colour, coverage and the two blends the parent
   * may chain through. Below is the group's blend base; Color and Coverage are its straight
   * result; Blend is the row blended at factor one (only the parent's premul chain uses it);
   * Result is the row laid over Below. */
  if (layer_group != nullptr && current.source != nullptr) {
    RowResult &result = r_result;
    const MaterialPaintChannelInfo &channel_info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    auto add_group_socket = [&](const char *kind,
                                const StringRef type,
                                const NodeTreeInterfaceSocketFlag flag)
        -> bNodeTreeInterfaceSocket * {
      char base[192];
      SNPRINTF(base, "%s %s", kind, channel_info.ui_name);
      return layer_group_add_socket(*layer_group, base, type, flag);
    };
    bNodeTreeInterfaceSocket *below_iface = add_group_socket(
        "Below", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT);
    bNodeTreeInterfaceSocket *color_iface = add_group_socket(
        "Color", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT);
    bNodeTreeInterfaceSocket *coverage_iface = add_group_socket(
        "Coverage", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT);
    bNodeTreeInterfaceSocket *blend_iface = add_group_socket(
        "Blend", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT);
    bNodeTreeInterfaceSocket *result_iface = add_group_socket(
        "Result", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT);
    /* A private scalar output for the content alpha. Only when this row tracks one (a Base
     * Color Paint leaf or a folder with tracked content); absent otherwise, so groups for
     * untracked rows keep the exact contract they had. */
    bNodeTreeInterfaceSocket *content_alpha_iface = nullptr;
    if (current.content_alpha != nullptr) {
      content_alpha_iface = add_group_socket(
          "Content Alpha", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT);
      if (content_alpha_iface == nullptr) {
        return false;
      }
    }
    if (below_iface == nullptr || color_iface == nullptr || coverage_iface == nullptr ||
        blend_iface == nullptr || result_iface == nullptr)
    {
      return false;
    }
    bNodeSocket *below = bke::node_find_socket(
        *group_input, SOCK_OUT, UString::from_ptr_noinline(below_iface->identifier));
    bNodeSocket *color_out = bke::node_find_socket(
        *layer_group->group_output,
        SOCK_IN,
        UString::from_ptr_noinline(color_iface->identifier));
    bNodeSocket *coverage_out = bke::node_find_socket(
        *layer_group->group_output,
        SOCK_IN,
        UString::from_ptr_noinline(coverage_iface->identifier));
    bNodeSocket *blend_out = bke::node_find_socket(
        *layer_group->group_output,
        SOCK_IN,
        UString::from_ptr_noinline(blend_iface->identifier));
    bNodeSocket *result_out = bke::node_find_socket(
        *layer_group->group_output,
        SOCK_IN,
        UString::from_ptr_noinline(result_iface->identifier));
    bNodeSocket *content_alpha_out = nullptr;
    if (content_alpha_iface != nullptr) {
      content_alpha_out = bke::node_find_socket(
          *layer_group->group_output,
          SOCK_IN,
          UString::from_ptr_noinline(content_alpha_iface->identifier));
    }
    if (below == nullptr || color_out == nullptr || coverage_out == nullptr ||
        blend_out == nullptr || result_out == nullptr ||
        (content_alpha_iface != nullptr && content_alpha_out == nullptr))
    {
      return false;
    }
    bke::node_add_link(tree, *current.source_node, *current.source,
                       *layer_group->group_output, *color_out);
    if (current.factor != nullptr && current.factor_node != nullptr) {
      bke::node_add_link(tree, *current.factor_node, *current.factor,
                         *layer_group->group_output, *coverage_out);
    }
    else if (coverage_out->default_value != nullptr) {
      static_cast<bNodeSocketValueFloat *>(coverage_out->default_value)->value = 1.0f;
    }
    if (content_alpha_out != nullptr) {
      bke::node_add_link(tree,
                         *current.content_alpha_node,
                         *current.content_alpha,
                         *layer_group->group_output,
                         *content_alpha_out);
    }

    /* blend(Below, Color) at factor one; the parent feeds Below from its own chain. */
    auto add_row_blend = [&](bNodeSocket *factor,
                             const float factor_default)
        -> std::pair<bNode *, bNodeSocket *> {
      if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && ctx.normal_combine_group != nullptr &&
          !BKE_paint_layers_normal_replace(*layer) && tree.typeinfo != nullptr &&
          tree.typeinfo->group_idname != nullptr)
      {
        bNode *combine = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
        if (combine == nullptr) {
          return {nullptr, nullptr};
        }
        combine->id = &ctx.normal_combine_group->id;
        id_us_plus(&ctx.normal_combine_group->id);
        combine->location[0] = location_x;
        combine->location[1] = location_y;
        nodes::update_node_declaration_and_sockets(tree, *combine);
        bNodeSocket *a_in = bke::node_find_socket(
            *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
        bNodeSocket *b_in = bke::node_find_socket(
            *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
        bNodeSocket *f_in = bke::node_find_socket(
            *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
        bNodeSocket *r_out = bke::node_find_socket(
            *combine, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
        if (a_in == nullptr || b_in == nullptr || f_in == nullptr || r_out == nullptr) {
          return {nullptr, nullptr};
        }
        bke::node_add_link(tree, *group_input, *below, *combine, *a_in);
        bke::node_add_link(
            tree, *current.source_node, *current.source, *combine, *b_in);
        if (factor != nullptr) {
          bke::node_add_link(tree, *current.factor_node, *factor, *combine, *f_in);
        }
        else if (f_in->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(f_in->default_value)->value = factor_default;
        }
        return {combine, r_out};
      }
      bNode *mix = mix_node_add(tree,
                                BKE_paint_layers_blend_to_ramp(
                                    eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(*layer, channel))),
                                location_x,
                                location_y);
      if (mix == nullptr) {
        return {nullptr, nullptr};
      }
      bNodeSocket *a_in = socket_in(*mix, "A_Color");
      bNodeSocket *b_in = socket_in(*mix, "B_Color");
      bNodeSocket *f_in = socket_in(*mix, "Factor_Float");
      bNodeSocket *r_out = socket_out(*mix, "Result_Color");
      if (a_in == nullptr || b_in == nullptr || f_in == nullptr || r_out == nullptr) {
        return {nullptr, nullptr};
      }
      bke::node_add_link(tree, *group_input, *below, *mix, *a_in);
      bke::node_add_link(tree, *current.source_node, *current.source, *mix, *b_in);
      if (factor != nullptr) {
        bke::node_add_link(tree, *current.factor_node, *factor, *mix, *f_in);
      }
      else if (f_in->default_value != nullptr) {
        static_cast<bNodeSocketValueFloat *>(f_in->default_value)->value = factor_default;
      }
      return {mix, r_out};
    };

    auto [result_node, result_source] = add_row_blend(current.factor, 1.0f);
    if (result_source == nullptr) {
      return false;
    }
    bke::node_add_link(
        tree, *result_node, *result_source, *layer_group->group_output, *result_out);
    if (premul) {
      auto [blend_node, blend_source] = add_row_blend(nullptr, 1.0f);
      if (blend_source != nullptr) {
        bke::node_add_link(
            tree, *blend_node, *blend_source, *layer_group->group_output, *blend_out);
      }
    }

    result.grouped = true;
    result.group_instance = layer_group->instance;
    result.group_below = bke::node_find_socket(
        *layer_group->instance, SOCK_IN, UString::from_ptr_noinline(below_iface->identifier));
    result.group_color = bke::node_find_socket(
        *layer_group->instance, SOCK_OUT, UString::from_ptr_noinline(color_iface->identifier));
    result.group_coverage = bke::node_find_socket(
        *layer_group->instance,
        SOCK_OUT,
        UString::from_ptr_noinline(coverage_iface->identifier));
    result.group_blend = bke::node_find_socket(
        *layer_group->instance, SOCK_OUT, UString::from_ptr_noinline(blend_iface->identifier));
    result.group_result = bke::node_find_socket(
        *layer_group->instance, SOCK_OUT, UString::from_ptr_noinline(result_iface->identifier));
    if (content_alpha_iface != nullptr) {
      result.group_content_alpha = bke::node_find_socket(
          *layer_group->instance,
          SOCK_OUT,
          UString::from_ptr_noinline(content_alpha_iface->identifier));
    }
  }
  return true;
}

}  // namespace bke::paint_layers
}  // namespace blender
