/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include <climits>
#include <cstdio>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"
#include "DNA_armature_types.h"
#include "DNA_collection_types.h"
#include "DNA_constraint_types.h"
#include "DNA_gpencil_legacy_types.h"
#include "DNA_image_types.h"
#include "DNA_key_types.h"
#include "DNA_layer_types.h"
#include "DNA_light_types.h"
#include "DNA_lightprobe_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_text_types.h"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLF_api.hh"
#include "BLT_translation.hh"

#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_deform.hh"
#include "BKE_global.hh"
#include "BKE_gpencil_legacy.h"
#include "BKE_grease_pencil.hh"
#include "BKE_icons.hh"
#include "BKE_idtype.hh"
#include "BKE_key.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_override.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_main_namemap.hh"
#include "BKE_material.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_object.hh"
#include "BKE_particle.h"
#include "BKE_preview_image.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "ANIM_armature.hh"
#include "ANIM_bone_collections.hh"
#include "ANIM_keyframing.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_armature.hh"
#include "ED_fileselect.hh"
#include "ED_id_management.hh"
#include "ED_outliner.hh"
#include "ED_render.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_types.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_state.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "UI_interface.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "../outliner_intern.hh"
#include "../tree/tree_element.hh"
#include "../tree/tree_element_grease_pencil_node.hh"
#include "../tree/tree_element_id.hh"
#include "../tree/tree_element_overrides.hh"
#include "../tree/tree_element_rna.hh"
#include "../tree/tree_element_seq.hh"
#include "../tree/tree_iterator.hh"
#include "outliner_rendered_preview.hh"

namespace blender {

namespace ed::outliner {

/* -------------------------------------------------------------------- */
/** \name Rendered previews
 *
 * A preview that is a sphere shaded with a description's values, or a material as it is now,
 * draws pixels the stock ID preview machinery produces. That machinery owns the whole render
 * lifecycle: the job, the debounce, the exclusivity with the other preview renders, and the flags
 * that say the pixels are stale. This side owns the pixels themselves and the asking -- a visible
 * preview asks, an unchanged one does not, so nothing renders until a preview is drawn and
 * nothing re-renders until what it shows has changed.
 * \{ */

namespace {

/** The pixels of one rendered preview, keyed by its hash (a sphere) or by its material (a
 * material, which renders into the same pixels again after an edit). Session state this module
 * owns: rebuilt on demand, never written into files or undo.
 */
struct RenderedPreviewEntry {
  PreviewImage *prv = nullptr;
  /** The #RenderedPreview::hash the held pixels were rendered for. */
  uint64_t rendered_hash = 0;
  /** The hash the tooltip-size pixels were asked for, kept apart because that size is only
   * rendered once a tooltip wants it. */
  uint64_t large_hash = 0;
};

static Map<uint64_t, RenderedPreviewEntry> g_rendered_previews;

/**
 * Keeps a pathological stack from accumulating spheres. Any policy subtler than dropping what has
 * gone quiet is not worth the bookkeeping for a cache this small: a dropped entry renders again on
 * its next draw, and a slot that is not drawn holds no entry at all.
 */
constexpr int RENDERED_PREVIEW_MAX = 256;

/** Side in pixels of the picture a slot's tooltip shows; the stock preview size is 128. */
constexpr int RENDERED_PREVIEW_TOOLTIP_SIZE = 256;

/** The key of a material's rendered source preview; the top bit keeps it apart from the hashes
 * the spheres are keyed by. */
static uint64_t rendered_preview_material_key(const uint32_t id_uid)
{
  return 0x8000000000000000ull | id_uid;
}

/** Drop the quiet entries, as #RENDERED_PREVIEW_MAX asks. An entry whose pixels a render job
 * is still writing keeps them: freeing them under the job would leave its worker writing into
 * freed memory, and the next pass once it is done takes them.
 */
static void rendered_previews_evict()
{
  if (g_rendered_previews.size() <= RENDERED_PREVIEW_MAX) {
    return;
  }
  Vector<uint64_t> dropped;
  for (const uint64_t key : g_rendered_previews.keys()) {
    RenderedPreviewEntry &entry = g_rendered_previews.lookup(key);
    if (entry.prv == nullptr ||
        (!BKE_previewimg_is_rendering(entry.prv, ICON_SIZE_ICON) &&
         !BKE_previewimg_is_rendering(entry.prv, ICON_SIZE_PREVIEW)))
    {
      BKE_previewimg_free(&entry.prv);
      dropped.append(key);
    }
  }
  for (const uint64_t key : dropped) {
    g_rendered_previews.remove(key);
  }
}

/** The material a sphere renders from, with its Principled BSDF at hand. The maps variant also
 * holds the map nodes of the render in flight: they are taken down again once the render holds
 * its own copy of the tree, so the template points at no image between renders and a file being
 * closed cannot leave it holding one.
 */
struct StackSphereTemplate {
  Material *material = nullptr;
  bNode *principled = nullptr;
  /** The texture and separate-colour nodes of the render in flight; empty between renders. */
  Vector<bNode *> helpers;
};

/** The Fill's sphere: shaded from the slot's own constants. */
static StackSphereTemplate g_stack_sphere_constants;
/** A Material row's sphere: shaded from the row's baked channel maps. */
static StackSphereTemplate g_stack_sphere_maps;

/** Socket by name, read straight off the DNA list the way #bke::node_find_socket reads it by
 * identifier: the runtime socket spans want the tree's topology cache, which a tree this simple
 * has no reason to keep warm. */
static bNodeSocket *stack_sphere_socket_find(bNode &node,
                                             const eNodeSocketInOut in_out,
                                             const char *name)
{
  ListBaseT<bNodeSocket> &sockets = (in_out == SOCK_IN) ? node.inputs : node.outputs;
  for (bNodeSocket &socket : sockets) {
    if (STREQ(socket.name, name)) {
      return &socket;
    }
  }
  return nullptr;
}

/**
 * The template's base: one Principled BSDF into one Material Output, built once in the preview
 * Main the render itself lives in and re-shaded per request. A material with no node tree renders
 * as the fixed default surface there, so the tree is what makes what the slot shows visible.
 */
static StackSphereTemplate *stack_sphere_template_ensure(StackSphereTemplate &tmplate,
                                                         const char *name)
{
  if (tmplate.material != nullptr) {
    return &tmplate;
  }
  /* The preview render applies the material it renders to the preview scene's objects in the
   * preview Main, so that is where the template belongs. Loading the base is idempotent, and the
   * draw runs on the main thread it asks for. */
  ED_preview_ensure_dbase(true);
  if (G.pr_main == nullptr) {
    return nullptr;
  }
  Material *material = BKE_material_add(G.pr_main, name);
  if (material == nullptr) {
    return nullptr;
  }
  tmplate.material = material;
  bNodeTree *tree = material->nodetree;
  if (tree == nullptr) {
    return nullptr;
  }
  bNode *principled = bke::node_add_static_node(nullptr, *tree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, *tree, SH_NODE_OUTPUT_MATERIAL);
  bNodeSocket *shader_out = principled != nullptr ?
                                stack_sphere_socket_find(*principled, SOCK_OUT, "BSDF") :
                                nullptr;
  bNodeSocket *surface_in = output != nullptr ?
                                stack_sphere_socket_find(*output, SOCK_IN, "Surface") :
                                nullptr;
  if (principled == nullptr || output == nullptr || shader_out == nullptr ||
      surface_in == nullptr)
  {
    return nullptr;
  }
  bke::node_add_link(*tree, *principled, *shader_out, *output, *surface_in);
  BKE_ntree_update_tag_all(tree);
  BKE_ntree_update_after_single_tree_change(*G.pr_main, *tree);
  tmplate.principled = principled;
  return &tmplate;
}

/**
 * Re-shade the constants template with what a Fill's slot shows. The socket colours are scene
 * linear -- the space the slot's colour already travels in -- so they take it as it comes; a
 * material's own RGB fields are display-referred and would have needed the conversion.
 */
static void stack_sphere_constants_values_set(const StackSphereTemplate &tmplate,
                                              const float color[4],
                                              const float metallic,
                                              const float roughness)
{
  bNode &principled = *tmplate.principled;
  if (bNodeSocket *socket = stack_sphere_socket_find(principled, SOCK_IN, "Base Color")) {
    if (socket->default_value != nullptr) {
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(socket->default_value)->value, color);
    }
  }
  if (bNodeSocket *socket = stack_sphere_socket_find(principled, SOCK_IN, "Metallic")) {
    if (socket->default_value != nullptr) {
      static_cast<bNodeSocketValueFloat *>(socket->default_value)->value = metallic;
    }
  }
  if (bNodeSocket *socket = stack_sphere_socket_find(principled, SOCK_IN, "Roughness")) {
    if (socket->default_value != nullptr) {
      static_cast<bNodeSocketValueFloat *>(socket->default_value)->value = roughness;
    }
  }
}

/** Take the maps template's helper nodes down, releasing their hold on the baked maps. The tree
 * is left as it was built: one Principled BSDF into one output, shading nothing but ready. */
static void stack_sphere_helpers_clear(StackSphereTemplate &tmplate)
{
  if (tmplate.material == nullptr || tmplate.material->nodetree == nullptr) {
    tmplate.helpers.clear();
    return;
  }
  bNodeTree &tree = *tmplate.material->nodetree;
  for (bNode *node : tmplate.helpers) {
    if (node != nullptr) {
      bke::node_remove_node(nullptr, tree, *node, true, false);
    }
  }
  tmplate.helpers.clear();
  BKE_ntree_update_tag_all(&tree);
  BKE_ntree_update_after_single_tree_change(*G.pr_main, tree);
}

/**
 * Wire the maps template for what a Material row's slot shows, replacing whatever the previous
 * render left wired. A channel with a map reads it through a texture node; the scalar channels
 * go by way of a Separate Color node, because a texture's colour output cannot reach a float
 * input directly. A channel without a map is left unlinked, so the socket's constant answers
 * with what the slot carries: the source's resolved colour, metallic and roughness.
 */
static void stack_sphere_maps_values_set(const bContext *C,
                                         StackSphereTemplate &tmplate,
                                         const float color[4],
                                         const float metallic,
                                         const float roughness,
                                         const uint32_t map_uid[3])
{
  bNodeTree &tree = *tmplate.material->nodetree;
  bNode &principled = *tmplate.principled;
  stack_sphere_helpers_clear(tmplate);
  Main *bmain = CTX_data_main(C);
  for (int i = 0; i < 3; i++) {
    const char *principled_in_name = (i == 0) ? "Base Color" : (i == 1) ? "Metallic" : "Roughness";
    bNodeSocket *principled_in = stack_sphere_socket_find(principled, SOCK_IN, principled_in_name);
    if (principled_in == nullptr) {
      continue;
    }
    Image *image = map_uid[i] != 0 ?
                       id_cast<Image *>(BKE_libblock_find_session_uid(bmain, ID_IM, map_uid[i])) :
                       nullptr;
    if (image == nullptr) {
      /* No map for the channel: the socket's constant answers. The constants template writes
       * this same way, with the same colours and the same resolved scalars. */
      if (i == 0) {
        if (principled_in->default_value != nullptr) {
          copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(principled_in->default_value)->value,
                     color);
        }
      }
      else if (principled_in->default_value != nullptr) {
        static_cast<bNodeSocketValueFloat *>(principled_in->default_value)->value =
            (i == 1) ? metallic : roughness;
      }
      continue;
    }
    bNode *texture = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
    bNodeSocket *texture_color = texture != nullptr ?
                                     stack_sphere_socket_find(*texture, SOCK_OUT, "Color") :
                                     nullptr;
    if (texture == nullptr || texture_color == nullptr) {
      continue;
    }
    /* The same hold the generated graph puts on a map, released when the helper comes down after
     * the render has taken its own copy of the tree. */
    texture->id = &image->id;
    id_us_plus(&image->id);
    tmplate.helpers.append(texture);
    if (i == 0) {
      bke::node_add_link(tree, *texture, *texture_color, principled, *principled_in);
      continue;
    }
    bNode *separate = bke::node_add_static_node(nullptr, tree, SH_NODE_SEPARATE_COLOR);
    bNodeSocket *separate_color = separate != nullptr ?
                                      stack_sphere_socket_find(*separate, SOCK_IN, "Color") :
                                      nullptr;
    bNodeSocket *separate_red = separate != nullptr ?
                                    stack_sphere_socket_find(*separate, SOCK_OUT, "Red") :
                                    nullptr;
    if (separate == nullptr || separate_color == nullptr || separate_red == nullptr) {
      continue;
    }
    tmplate.helpers.append(separate);
    bke::node_add_link(tree, *texture, *texture_color, *separate, *separate_color);
    bke::node_add_link(tree, *separate, *separate_red, principled, *principled_in);
  }
  BKE_ntree_update_tag_all(&tree);
  BKE_ntree_update_after_single_tree_change(*G.pr_main, tree);
}

/**
 * Ask the stock machinery for \a size pixels of the sphere \a prv holds the slot of. The template
 * is written first: the render copies it on this thread before its job starts, so the worker
 * renders the values just written, whatever sphere is being rendered beside it. Returns false when
 * no template can be built this session.
 */
static bool stack_sphere_render_ask(const bContext *C,
                                    PreviewImage *prv,
                                    const float color[4],
                                    const float metallic,
                                    const float roughness,
                                    const uint32_t map_uid[3],
                                    const eIconSizes size)
{
  /* A slot with any map shades from the maps (a Material row's content); one without shades
   * from its own constants (a Fill). A source with only a roughness texture has no base map. */
  const bool has_maps = (map_uid[0] != 0 || map_uid[1] != 0 || map_uid[2] != 0);
  StackSphereTemplate *tmplate = has_maps ?
                                     stack_sphere_template_ensure(
                                         g_stack_sphere_maps, "Stack Layer Sphere Maps Preview") :
                                     stack_sphere_template_ensure(
                                         g_stack_sphere_constants, "Stack Layer Sphere Preview");
  if (tmplate == nullptr || tmplate->principled == nullptr || tmplate->material == nullptr) {
    return false;
  }
  if (has_maps) {
    stack_sphere_maps_values_set(C, *tmplate, color, metallic, roughness, map_uid);
  }
  else {
    stack_sphere_constants_values_set(*tmplate, color, metallic, roughness);
  }
  if (prv->rect[size] != nullptr) {
    /* Pixels of an earlier ask already sit here: flag them stale so the stock machinery
     * re-renders in place, debounced the way it debounces its own icons. */
    prv->flag[size] |= PRV_CHANGED;
  }
  ui::icon_render_id_ex(C, nullptr, &tmplate->material->id, size, true, prv);
  if (has_maps) {
    /* The render holds its own copy of the tree, so the helpers' hold on the maps can come off. */
    stack_sphere_helpers_clear(*tmplate);
  }
  return true;
}

/**
 * Whether the size's pixels can be drawn: allocated, not flagged stale, and not being re-rendered
 * this moment. A size that was asked for shows the caller's stand-in until this turns true.
 */
static bool rendered_preview_pixels_ready(const PreviewImage &prv, const eIconSizes size)
{
  return prv.rect[size] != nullptr && prv.w[size] > 0 && (prv.flag[size] & PRV_CHANGED) == 0 &&
         !BKE_previewimg_is_rendering(&prv, size);
}

/** The frame the placeholder and the swatch draw around their slot, drawn the same around pixels. */
static void rendered_preview_frame_draw(const rctf &preview_rect, const float alpha_fac)
{
  float preview_border[4];
  ui::theme::get_color_blend_4f(TH_TEXT, TH_BACK, 0.9f, preview_border);
  preview_border[3] *= alpha_fac;
  const float margin = OUTLINER_STACK_PREVIEW_FRAME_MARGIN * UI_SCALE_FAC;
  rctf frame{};
  BLI_rctf_init(&frame,
                preview_rect.xmin - margin,
                preview_rect.xmax + margin,
                preview_rect.ymin - margin,
                preview_rect.ymax + margin);
  draw_roundbox_corner_set(ui::CNR_ALL);
  ui::draw_roundbox_aa(&frame, false, UI_UNIT_Y / 5.0f, preview_border);
  GPU_blend(GPU_BLEND_ALPHA); /* Round-box disables. */
}

}  // namespace

static bool rendered_preview_id_draw(const bContext *C,
                           const rctf &preview_rect,
                           const uint32_t id_uid,
                           const short id_type,
                           const float color[4],
                           const float alpha_fac)
{
  ID *id = BKE_libblock_find_session_uid(CTX_data_main(C), id_type, id_uid);
  if (id == nullptr || !ED_preview_id_render_is_supported(id)) {
    return false;
  }
  PreviewImage *prv = BKE_previewimg_id_ensure(id);
  const int icon_id = BKE_icon_id_ensure(id);
  if (prv == nullptr || icon_id == 0) {
    return false;
  }
  if (rendered_preview_pixels_ready(*prv, ICON_SIZE_ICON)) {
    ui::icon_draw_ex(preview_rect.xmin,
                     preview_rect.ymin,
                     icon_id,
                     UI_INV_SCALE_FAC,
                     alpha_fac,
                     0.0f,
                     nullptr,
                     false,
                     nullptr,
                     false,
                     OUTLINER_STACK_PREVIEW_SCALE);
    rendered_preview_frame_draw(preview_rect, alpha_fac);
    return true;
  }
  /* The stock icon machinery starts the render job itself, and only when the pixels are missing
   * or flagged changed -- this is what a picker's thumbnail makes of every redraw, so asking from
   * the draw costs nothing while the pixels answer. The data-block's own change flag covers its
   * edits: the slot needs no hash of its own for this kind. */
  ui::icon_ensure_deferred(C, icon_id, false);
  stack_preview_color_draw(preview_rect, color, 1.0f);
  return true;
}

static bool rendered_preview_sphere_draw(const bContext *C,
                               const rctf &preview_rect,
                               const float color[4],
                               const float metallic,
                               const float roughness,
                               const uint32_t map_uid[3],
                               const uint64_t hash,
                               const float alpha_fac)
{
  rendered_previews_evict();
  RenderedPreviewEntry &entry = g_rendered_previews.lookup_or_add_default(hash);
  if (entry.prv == nullptr) {
    entry.prv = BKE_previewimg_create();
  }
  if (entry.rendered_hash == hash && rendered_preview_pixels_ready(*entry.prv, ICON_SIZE_ICON)) {
    const int icon_id = BKE_icon_preview_ensure(nullptr, entry.prv);
    if (icon_id != 0) {
      ui::icon_draw_ex(preview_rect.xmin,
                       preview_rect.ymin,
                       icon_id,
                       UI_INV_SCALE_FAC,
                       alpha_fac,
                       0.0f,
                       nullptr,
                       false,
                       nullptr,
                       false,
                       OUTLINER_STACK_PREVIEW_SCALE);
      rendered_preview_frame_draw(preview_rect, alpha_fac);
      return true;
    }
  }
  /* Ask for the sphere only when it was never asked for or what it shows has changed: while a
   * render for this hash is in flight there is nothing to ask, and a re-ask on every redraw would
   * restart the job on every one of them. */
  const bool render_needed = entry.rendered_hash != hash ||
                             (entry.prv->rect[ICON_SIZE_ICON] == nullptr &&
                              !BKE_previewimg_is_rendering(entry.prv, ICON_SIZE_ICON));
  if (render_needed) {
    if (!stack_sphere_render_ask(C, entry.prv, color, metallic, roughness, map_uid, ICON_SIZE_ICON))
    {
      /* No template can be built this session; the flat swatch answers for the slot instead. */
      return false;
    }
    entry.rendered_hash = hash;
  }
  stack_preview_color_draw(preview_rect, color, 1.0f);
  return true;
}

static bool rendered_preview_material_draw(const bContext *C,
                               const rctf &preview_rect,
                               const uint32_t id_uid,
                               const float color[4],
                               const uint64_t hash,
                               const float alpha_fac)
{
  ID *id = BKE_libblock_find_session_uid(CTX_data_main(C), ID_MA, id_uid);
  if (id == nullptr || !ED_preview_id_render_is_supported(id)) {
    return false;
  }
  rendered_previews_evict();
  /* Held per material rather than per hash: an edit of its graph renders into the same pixels, so
   * the previous render stays on show until the new one lands. */
  RenderedPreviewEntry &entry = g_rendered_previews.lookup_or_add_default(
      rendered_preview_material_key(id_uid));
  if (entry.prv == nullptr) {
    entry.prv = BKE_previewimg_create();
  }
  /* As for the sphere: ask once per hash, never on every redraw while the job is in flight. */
  const bool render_needed = entry.rendered_hash != hash ||
                             (entry.prv->rect[ICON_SIZE_ICON] == nullptr &&
                              !BKE_previewimg_is_rendering(entry.prv, ICON_SIZE_ICON));
  if (render_needed) {
    if (entry.prv->rect[ICON_SIZE_ICON] != nullptr) {
      entry.prv->flag[ICON_SIZE_ICON] |= PRV_CHANGED;
    }
    entry.rendered_hash = hash;
    ui::icon_render_id_ex(C, nullptr, id, ICON_SIZE_ICON, true, entry.prv);
  }
  if (entry.prv->rect[ICON_SIZE_ICON] != nullptr && entry.prv->w[ICON_SIZE_ICON] > 0) {
    const int icon_id = BKE_icon_preview_ensure(nullptr, entry.prv);
    if (icon_id != 0) {
      ui::icon_draw_ex(preview_rect.xmin,
                       preview_rect.ymin,
                       icon_id,
                       UI_INV_SCALE_FAC,
                       alpha_fac,
                       0.0f,
                       nullptr,
                       false,
                       nullptr,
                       false,
                       OUTLINER_STACK_PREVIEW_SCALE);
      rendered_preview_frame_draw(preview_rect, alpha_fac);
      return true;
    }
  }
  /* Before the first render lands, the pixels the material already carries -- for an asset the
   * thumbnail it came with -- stand in, so the user sees what was added. They are drawn as they
   * are, and nothing is asked of the stock machinery: it would render over them. */
  const PreviewImage *id_prv = BKE_previewimg_id_ensure(id);
  const int icon_id = BKE_icon_id_ensure(id);
  if (id_prv != nullptr && icon_id != 0 && id_prv->rect[ICON_SIZE_ICON] != nullptr &&
      id_prv->w[ICON_SIZE_ICON] > 0)
  {
    ui::icon_draw_ex(preview_rect.xmin,
                     preview_rect.ymin,
                     icon_id,
                     UI_INV_SCALE_FAC,
                     alpha_fac,
                     0.0f,
                     nullptr,
                     false,
                     nullptr,
                     false,
                     OUTLINER_STACK_PREVIEW_SCALE);
    rendered_preview_frame_draw(preview_rect, alpha_fac);
    return true;
  }
  stack_preview_color_draw(preview_rect, color, 1.0f);
  return true;
}

bool rendered_preview_draw(const bContext *C,
                           const rctf &preview_rect,
                           const RenderedPreview &preview,
                           const float alpha_fac)
{
  switch (preview.kind) {
    case RenderedPreviewKind::Id:
      return rendered_preview_id_draw(
          C, preview_rect, preview.id_uid, preview.id_type, preview.color, alpha_fac);
    case RenderedPreviewKind::Sphere:
      return rendered_preview_sphere_draw(C,
                                          preview_rect,
                                          preview.color,
                                          preview.metallic,
                                          preview.roughness,
                                          preview.map_uid,
                                          preview.hash,
                                          alpha_fac);
    case RenderedPreviewKind::Material:
      return rendered_preview_material_draw(
          C, preview_rect, preview.id_uid, preview.color, preview.hash, alpha_fac);
  }
  return false;
}

void rendered_preview_tooltip_add(ui::Block *block,
                                  const rctf &preview_rect,
                                  const RenderedPreview &preview)
{
  /* An ID preview keeps the data-block's own tooltip machinery. */
  if (preview.kind == RenderedPreviewKind::Id) {
    return;
  }
  ui::Button *but = uiDefIconBut(block,
                                 ui::ButtonType::Label,
                                 ICON_NONE,
                                 int(preview_rect.xmin),
                                 int(preview_rect.ymin),
                                 int(BLI_rctf_size_x(&preview_rect)),
                                 int(BLI_rctf_size_y(&preview_rect)),
                                 nullptr,
                                 0.0,
                                 0.0,
                                 "");
  if (but == nullptr) {
    return;
  }
  /* Captured by value: the block is rebuilt every draw, the rows with it. */
  ui::button_func_tooltip_custom_set_cpp(*but, [preview](bContext &C, ui::TooltipData &tip) {
    const std::string title = preview.label.empty() ? std::string(IFACE_("Preview")) : preview.label;
    ui::tooltip_text_field_add(tip, title, {}, ui::TIP_STYLE_HEADER, ui::TIP_LC_MAIN);
    RenderedPreviewEntry *entry = g_rendered_previews.lookup_ptr(
        preview.kind == RenderedPreviewKind::Material ?
            rendered_preview_material_key(preview.id_uid) :
            preview.hash);
    if (entry == nullptr || entry->prv == nullptr || entry->rendered_hash != preview.hash) {
      return;
    }
    PreviewImage *prv = entry->prv;
    /* The slot renders at icon size; the larger size is asked for when a tooltip first wants it
     * for this hash and shown once it has landed, the icon-size pixels standing in meanwhile.
     * "Finished" is true of a size never rendered, so the pixels themselves are what is looked
     * at. */
    const auto has_pixels = [prv](const eIconSizes size) {
      return prv->rect[size] != nullptr && prv->w[size] > 0 && prv->h[size] > 0 &&
             !BKE_previewimg_is_rendering(prv, size);
    };
    if (entry->large_hash != preview.hash && !BKE_previewimg_is_rendering(prv, ICON_SIZE_PREVIEW))
    {
      entry->large_hash = preview.hash;
      if (prv->rect[ICON_SIZE_PREVIEW] != nullptr) {
        prv->flag[ICON_SIZE_PREVIEW] |= PRV_CHANGED;
      }
      else {
        /* The render takes its size from the buffer it finds, and would make the stock 128 pixel
         * one: too small to judge a material by. Its own, twice that, is made here. */
        prv->w[ICON_SIZE_PREVIEW] = RENDERED_PREVIEW_TOOLTIP_SIZE;
        prv->h[ICON_SIZE_PREVIEW] = RENDERED_PREVIEW_TOOLTIP_SIZE;
        prv->rect[ICON_SIZE_PREVIEW] = MEM_new_array_zeroed<uint>(
            size_t(RENDERED_PREVIEW_TOOLTIP_SIZE) * RENDERED_PREVIEW_TOOLTIP_SIZE, "prv_rect");
      }
      if (preview.kind == RenderedPreviewKind::Material) {
        ID *id = BKE_libblock_find_session_uid(CTX_data_main(&C), ID_MA, preview.id_uid);
        if (id != nullptr) {
          ui::icon_render_id_ex(&C, nullptr, id, ICON_SIZE_PREVIEW, true, prv);
        }
      }
      else {
        stack_sphere_render_ask(&C,
                                prv,
                                preview.color,
                                preview.metallic,
                                preview.roughness,
                                preview.map_uid,
                                ICON_SIZE_PREVIEW);
      }
    }
    /* Large pixels of an earlier hash are never shown for this one. */
    const eIconSizes size = (has_pixels(ICON_SIZE_PREVIEW) &&
                             entry->large_hash == preview.hash &&
                             (prv->flag[ICON_SIZE_PREVIEW] & PRV_CHANGED) == 0) ?
                                ICON_SIZE_PREVIEW :
                                ICON_SIZE_ICON;
    if (!has_pixels(size)) {
      return;
    }
    ImBuf *ibuf = BKE_previewimg_to_imbuf(prv, size);
    if (ibuf == nullptr) {
      return;
    }
    ui::TooltipImage image_data;
    image_data.ibuf = ibuf;
    image_data.width = short(float(ibuf->x) * UI_SCALE_FAC);
    image_data.height = short(float(ibuf->y) * UI_SCALE_FAC);
    image_data.border = true;
    image_data.background = ui::TooltipImageBackground::Checkerboard_Themed;
    image_data.premultiplied = true;
    ui::tooltip_text_field_add(tip, {}, {}, ui::TIP_STYLE_SPACER, ui::TIP_LC_NORMAL);
    ui::tooltip_text_field_add(tip, {}, {}, ui::TIP_STYLE_SPACER, ui::TIP_LC_NORMAL);
    ui::tooltip_image_field_add(tip, image_data);
    IMB_freeImBuf(ibuf);
  });
}

/** \} */

}  // namespace ed::outliner
}  // namespace blender
