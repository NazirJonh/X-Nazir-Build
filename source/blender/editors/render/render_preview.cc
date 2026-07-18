/* SPDX-FileCopyrightText: Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 */

/* global includes */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifndef WIN32
#  include <unistd.h>
#else
#  include <io.h>
#endif
#include "MEM_guardedalloc.h"

#include "BLI_math_matrix_c.hh"
#include "BLI_math_rotation_c.hh"
#include "BLI_rect.hh"
#include "BLI_set.hh"
#include "BLI_string_utf8.hh"
#include "BLI_task_c.hh"
#include "BLI_utildefines.hh"

#include "BLT_translation.hh"

#include "BLO_readfile.hh"

#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_texture_types.h"
#include "DNA_world_types.h"

#include "BKE_animsys.hh"
#include "BKE_armature.hh"
#include "BKE_brush.hh"
#include "BKE_collection.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_icons.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_light.h"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_object.hh"
#include "BKE_pose_backup.h"
#include "BKE_preview_image.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"
#include "BKE_texture.h"
#include "BKE_world.h"

#include "BLI_math_vector_c.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"
#include "IMB_thumbs.hh"

#include "BIF_glutil.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_shader.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "RE_engine.h"
#include "RE_pipeline.h"
#include "RE_texture.h"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_datafiles.h"
#include "ED_render.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"
#include "ED_view3d_offscreen.hh"

#include "UI_interface_icons.hh"

#include "ANIM_action.hh"
#include "ANIM_pose.hh"

namespace blender {

#ifndef NDEBUG
/* Used for database init assert(). */
#  include "BLI_threads.hh"
#endif

static void icon_copy_rect(const ImBuf *ibuf, uint w, uint h, uint *rect);

/* -------------------------------------------------------------------- */
/** \name Local Structs
 * \{ */

struct ShaderPreview {
  /* from wmJob */
  const void *owner;
  bool *stop, *do_update;

  Scene *scene;
  ID *id, *id_copy;
  ID *parent;
  MTex *slot;

  /* Data-blocks with nodes need full copy during preview render, GLSL uses it too. */
  Material *matcopy;
  Tex *texcopy;
  Light *lampcopy;
  World *worldcopy;

  /** Copy of the active objects #Object.color */
  float color[4];

  int sizex, sizey;
  uint *pr_rect;
  ePreviewRenderMethod pr_method;
  bool own_id_copy;

  Main *bmain;
  Main *pr_main;
};

struct IconPreview {
  Main *bmain;
  Depsgraph *depsgraph; /* May be nullptr (see #WM_OT_previews_ensure). */
  Scene *scene;
  void *owner;
  /** May be nullptr! (see #ICON_TYPE_PREVIEW case in #icon_ensure_deferred()). */
  ID *id;
  ID *id_copy;
  /* Which icon sizes to render. */
  bool render_size[NUM_ICON_SIZES];

  /* May be nullptr, is used for rendering IDs that require some other object for it to be applied
   * on before the ID can be represented as an image, for example when rendering an Action. */
  Object *active_object;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Preview for Buttons
 * \{ */

static Main *G_pr_main_grease_pencil = nullptr;

#ifndef WITH_HEADLESS
static Main *load_main_from_memory(const void *blend, int blend_size)
{
  const int fileflags = G.fileflags;
  Main *bmain = nullptr;
  BlendFileData *bfd;

  G.fileflags |= G_FILE_NO_UI;
  bfd = BLO_read_from_memory(blend, blend_size, BLO_READ_SKIP_NONE, nullptr);
  if (bfd) {
    bmain = bfd->main;
    MEM_delete(bfd);
  }
  G.fileflags = fileflags;

  return bmain;
}
#endif

void ED_preview_ensure_dbase(const bool with_gpencil)
{
#ifndef WITH_HEADLESS
  static bool base_initialized = false;
  static bool base_initialized_gpencil = false;
  BLI_assert(BLI_thread_is_main());
  if (!base_initialized) {
    G.pr_main = load_main_from_memory(datatoc_preview_blend, datatoc_preview_blend_size);
    base_initialized = true;
  }
  if (!base_initialized_gpencil && with_gpencil) {
    G_pr_main_grease_pencil = load_main_from_memory(datatoc_preview_grease_pencil_blend,
                                                    datatoc_preview_grease_pencil_blend_size);
    base_initialized_gpencil = true;
  }
#else
  UNUSED_VARS(with_gpencil);
#endif
}

bool ED_check_engine_supports_preview(const Scene *scene)
{
  RenderEngineType *type = RE_engines_find(scene->r.engine);
  return (type->flag & RE_USE_PREVIEW) != 0;
}

static bool preview_method_is_render(const ePreviewRenderMethod pr_method)
{
  return ELEM(pr_method, PR_ICON_RENDER, PR_BUTS_RENDER);
}

void ED_preview_free_dbase()
{
  if (G.pr_main) {
    BKE_main_free(G.pr_main);
  }

  if (G_pr_main_grease_pencil) {
    BKE_main_free(G_pr_main_grease_pencil);
  }
}

static Scene *preview_get_scene(Main *pr_main)
{
  if (pr_main == nullptr) {
    return nullptr;
  }

  return static_cast<Scene *>(pr_main->scenes.first);
}

const char *ED_preview_collection_name(const ePreviewType pr_type)
{
  switch (pr_type) {
    case MA_FLAT:
      return "Flat";
    case MA_SPHERE:
      return "Sphere";
    case MA_CUBE:
      return "Cube";
    case MA_SHADERBALL:
      return "Shader Ball";
    case MA_CLOTH:
      return "Cloth";
    case MA_FLUID:
      return "Fluid";
    case MA_SPHERE_A:
      return "World Sphere";
    case MA_LAMP:
      return "Lamp";
    case MA_SKY:
      return "Sky";
    case MA_HAIR:
      return "Hair";
    case MA_ATMOS:
      return "Atmosphere";
    default:
      BLI_assert_msg(0, "Unknown preview type");
      return "";
  }
}

static bool render_engine_supports_ray_visibility(const Scene *sce)
{
  return !STREQ(sce->r.engine, RE_engine_id_BLENDER_EEVEE);
}

static void switch_preview_collection_visibility(ViewLayer *view_layer, const ePreviewType pr_type)
{
  /* Set appropriate layer as visible. */
  LayerCollection *lc = static_cast<LayerCollection *>(view_layer->layer_collections.first);
  const char *collection_name = ED_preview_collection_name(pr_type);

  for (lc = static_cast<LayerCollection *>(lc->layer_collections.first); lc; lc = lc->next) {
    if (STREQ(lc->collection->id.name + 2, collection_name)) {
      lc->collection->flag &= ~COLLECTION_HIDE_RENDER;
    }
    else {
      lc->collection->flag |= COLLECTION_HIDE_RENDER;
    }
  }
}

static const char *preview_floor_material_name(const Scene *scene,
                                               const ePreviewRenderMethod pr_method)
{
  if (pr_method == PR_ICON_RENDER && render_engine_supports_ray_visibility(scene)) {
    return "FloorHidden";
  }
  return "Floor";
}

static void switch_preview_floor_material(Main *pr_main,
                                          Mesh *mesh,
                                          const Scene *scene,
                                          const ePreviewRenderMethod pr_method)
{
  if (mesh->totcol == 0) {
    return;
  }

  const char *material_name = preview_floor_material_name(scene, pr_method);
  Material *mat = static_cast<Material *>(
      BLI_findstring(&pr_main->materials, material_name, offsetof(ID, name) + 2));
  if (mat) {
    mesh->mat[0] = mat;
  }
}

static void switch_preview_floor_visibility(Main *pr_main,
                                            const Scene *scene,
                                            ViewLayer *view_layer,
                                            const ePreviewRenderMethod pr_method)
{
  /* Hide floor for icon renders. */
  BKE_view_layer_synced_ensure(*pr_main, scene, view_layer);
  for (Base &base : *BKE_view_layer_object_bases_get(view_layer)) {
    if (STREQ(base.object->id.name + 2, "Floor")) {
      base.object->visibility_flag &= ~OB_HIDE_RENDER;
      if (pr_method == PR_ICON_RENDER) {
        if (!render_engine_supports_ray_visibility(scene)) {
          base.object->visibility_flag |= OB_HIDE_RENDER;
        }
      }
      if (base.object->type == OB_MESH) {
        switch_preview_floor_material(
            pr_main, id_cast<Mesh *>(base.object->data), scene, pr_method);
      }
    }
  }
}

void ED_preview_set_visibility(Main *pr_main,
                               Scene *scene,
                               ViewLayer *view_layer,
                               const ePreviewType pr_type,
                               const ePreviewRenderMethod pr_method)
{
  switch_preview_collection_visibility(view_layer, pr_type);
  switch_preview_floor_visibility(pr_main, scene, view_layer, pr_method);
  BKE_layer_collection_sync(*pr_main, scene, view_layer);
}

static World *preview_get_localized_world(ShaderPreview *sp, World *world)
{
  if (world == nullptr) {
    return nullptr;
  }
  if (sp->worldcopy != nullptr) {
    return sp->worldcopy;
  }

  ID *id_copy = BKE_id_copy_ex(nullptr,
                               &world->id,
                               nullptr,
                               LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE |
                                   LIB_ID_COPY_NO_ANIMDATA);
  sp->worldcopy = id_cast<World *>(id_copy);
  BLI_addtail(&sp->pr_main->worlds, sp->worldcopy);
  return sp->worldcopy;
}

World *ED_preview_prepare_world_simple(Main *bmain)
{
  using namespace blender::bke;

  World *world = BKE_world_add(bmain, "SimpleWorld");
  bNodeTree *ntree = world->nodetree;

  bNode *background = node_add_node(nullptr, *ntree, "ShaderNodeBackground"_ustr);
  bNode *output = node_add_node(nullptr, *ntree, "ShaderNodeOutputWorld"_ustr);
  node_add_link(*world->nodetree,
                *background,
                *node_find_socket(*background, SOCK_OUT, "Background"_ustr),
                *output,
                *node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  node_set_active(*ntree, *output);

  world->nodetree = ntree;
  return world;
}

void ED_preview_world_simple_set_rgb(World *world, const float color[4])
{
  BLI_assert(world != nullptr);

  bNode *background = bke::node_find_node_by_name(*world->nodetree, "Background");
  BLI_assert(background != nullptr);

  auto *color_socket = static_cast<bNodeSocketValueRGBA *>(
      bke::node_find_socket(*background, SOCK_IN, "Color"_ustr)->default_value);
  copy_v4_v4(color_socket->value, color);
}

static ID *duplicate_ids(ID *id, const bool allow_failure)
{
  if (id == nullptr) {
    /* Non-ID preview render. */
    return nullptr;
  }

  switch (GS(id->name)) {
    case ID_OB:
    case ID_MA:
    case ID_TE:
    case ID_LA:
    case ID_WO: {
      BLI_assert(BKE_previewimg_id_supports_jobs(id));
      ID *id_copy = BKE_id_copy_ex(nullptr,
                                   id,
                                   nullptr,
                                   LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE |
                                       LIB_ID_COPY_NO_ANIMDATA);
      return id_copy;
    }
    case ID_GR: {
      /* Doesn't really duplicate the collection. Just creates a collection instance empty. */
      BLI_assert(BKE_previewimg_id_supports_jobs(id));
      Object *instance_empty = BKE_object_add_only_object(nullptr, OB_EMPTY, nullptr);
      instance_empty->instance_collection = id_cast<Collection *>(id);
      instance_empty->transflag |= OB_DUPLICOLLECTION;
      return &instance_empty->id;
    }
    /* These support threading, but don't need duplicating. */
    case ID_IM:
      BLI_assert(BKE_previewimg_id_supports_jobs(id));
      return nullptr;
    default:
      if (!allow_failure) {
        BLI_assert_msg(0, "ID type preview not supported.");
      }
      return nullptr;
  }
}

static const char *preview_world_name(const Scene *sce,
                                      const ID_Type id_type,
                                      const ePreviewRenderMethod pr_method)
{
  /* When rendering material icons the floor will not be shown in the output. Cycles will use a
   * material trick to show the floor in the reflections, but hide the floor for camera rays. For
   * Eevee we use a transparent world that has a projected grid.
   *
   * In the future when Eevee supports VULKAN ray-tracing we can re-evaluate and perhaps remove
   * this approximation.
   */
  if (id_type == ID_MA && pr_method == PR_ICON_RENDER &&
      !render_engine_supports_ray_visibility(sce))
  {
    return "WorldFloor";
  }
  return "World";
}

static World *preview_get_world(Main *pr_main,
                                const Scene *sce,
                                const ID_Type id_type,
                                const ePreviewRenderMethod pr_method)
{
  World *result = nullptr;
  const char *world_name = preview_world_name(sce, id_type, pr_method);
  result = static_cast<World *>(
      BLI_findstring(&pr_main->worlds, world_name, offsetof(ID, name) + 2));

  /* No world found return first world. */
  if (result == nullptr) {
    result = static_cast<World *>(pr_main->worlds.first);
  }

  BLI_assert_msg(result, "Preview file has no world.");
  return result;
}

static void preview_sync_exposure(World *dst, const World *src)
{
  BLI_assert(dst);
  BLI_assert(src);
  dst->exp = src->exp;
  dst->range = src->range;
}

World *ED_preview_prepare_world(Main *pr_main,
                                const Scene *scene,
                                const World *world,
                                const ID_Type id_type,
                                const ePreviewRenderMethod pr_method)
{
  World *result = preview_get_world(pr_main, scene, id_type, pr_method);
  if (world) {
    preview_sync_exposure(result, world);
  }
  return result;
}

/* call this with a pointer to initialize preview scene */
/* call this with nullptr to restore assigned ID pointers in preview scene */
static Scene *preview_prepare_scene(
    Main *bmain, Scene *scene, ID *id, int id_type, ShaderPreview *sp)
{
  Scene *sce;
  Main *pr_main = sp->pr_main;

  memcpy(pr_main->filepath, BKE_main_blendfile_path(bmain), sizeof(pr_main->filepath));

  sce = preview_get_scene(pr_main);
  if (sce) {
    ViewLayer *view_layer = static_cast<ViewLayer *>(sce->view_layers.first);

    /* Only enable the combined render-pass. */
    view_layer->passflag = SCE_PASS_COMBINED;
    view_layer->eevee.render_passes = eViewLayerEEVEEPassType{};

    /* This flag tells render to not execute depsgraph or F-Curves etc. */
    sce->r.scemode |= R_BUTS_PREVIEW;
    STRNCPY_UTF8(sce->r.engine, scene->r.engine);

    sce->r.color_mgt_flag = scene->r.color_mgt_flag;
    BKE_color_managed_display_settings_copy(&sce->display_settings, &scene->display_settings);

    BKE_color_managed_view_settings_free(&sce->view_settings);
    BKE_color_managed_view_settings_copy(&sce->view_settings, &scene->view_settings);

    if ((id && sp->pr_method == PR_ICON_RENDER) && id_type != ID_WO) {
      sce->r.alphamode = R_ALPHAPREMUL;
    }
    else {
      sce->r.alphamode = R_ADDSKY;
    }

    sce->r.cfra = scene->r.cfra;

    /* Setup the world. */
    sce->world = ED_preview_prepare_world(
        pr_main, sce, scene->world, static_cast<ID_Type>(id_type), sp->pr_method);

    if (id_type == ID_TE) {
      /* Texture is not actually rendered with engine, just set dummy value. */
      STRNCPY_UTF8(sce->r.engine, RE_engine_id_BLENDER_EEVEE);
    }

    if (id_type == ID_MA) {
      Material *mat = nullptr, *origmat = id_cast<Material *>(id);

      if (origmat) {
        /* work on a copy */
        BLI_assert(sp->id_copy != nullptr);
        mat = sp->matcopy = id_cast<Material *>(sp->id_copy);
        sp->id_copy = nullptr;
        BLI_addtail(&pr_main->materials, mat);

        /* Use current scene world for lighting. */
        if (mat->pr_flag == MA_PREVIEW_WORLD && sp->pr_method == PR_BUTS_RENDER) {
          /* Use current scene world to light sphere. */
          sce->world = preview_get_localized_world(sp, scene->world);
        }
        else if (sce->world && sp->pr_method != PR_ICON_RENDER) {
          /* Use a default world color. Using the current
           * scene world can be slow if it has big textures. */
          sce->world = ED_preview_prepare_world_simple(pr_main);

          /* Use brighter world color for grease pencil. */
          if (sp->pr_main == G_pr_main_grease_pencil) {
            const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            ED_preview_world_simple_set_rgb(sce->world, white);
          }
          else {
            const float dark[4] = {0.05f, 0.05f, 0.05f, 0.05f};
            ED_preview_world_simple_set_rgb(sce->world, dark);
          }
        }

        const ePreviewType preview_type = static_cast<ePreviewType>(mat->pr_type);
        ED_preview_set_visibility(pr_main, sce, view_layer, preview_type, sp->pr_method);
      }
      else {
        sce->display.render_aa = SCE_DISPLAY_AA_OFF;
      }
      BKE_view_layer_synced_ensure(*pr_main, sce, view_layer);
      for (Base &base : *BKE_view_layer_object_bases_get(view_layer)) {
        if (base.object->id.name[2] == 'p') {
          /* copy over object color, in case material uses it */
          copy_v4_v4(base.object->color, sp->color);

          if (OB_TYPE_SUPPORT_MATERIAL(base.object->type)) {
            /* don't use BKE_object_material_assign, it changed mat->id.us, which shows in the UI
             */
            Material ***matar = BKE_object_material_array_p(base.object);
            int actcol = max_ii(base.object->actcol - 1, 0);

            if (matar && actcol < base.object->totcol) {
              (*matar)[actcol] = mat;
            }
          }
          else if (base.object->type == OB_LAMP) {
            base.flag |= BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT;
          }
        }
      }
    }
    else if (id_type == ID_TE) {
      Tex *tex = nullptr, *origtex = id_cast<Tex *>(id);

      if (origtex) {
        BLI_assert(sp->id_copy != nullptr);
        tex = sp->texcopy = id_cast<Tex *>(sp->id_copy);
        sp->id_copy = nullptr;
        BLI_addtail(&pr_main->textures, tex);
      }
    }
    else if (id_type == ID_LA) {
      Light *la = nullptr, *origla = id_cast<Light *>(id);

      /* work on a copy */
      if (origla) {
        BLI_assert(sp->id_copy != nullptr);
        la = sp->lampcopy = id_cast<Light *>(sp->id_copy);
        sp->id_copy = nullptr;
        BLI_addtail(&pr_main->lights, la);
      }

      ED_preview_set_visibility(pr_main, sce, view_layer, MA_LAMP, sp->pr_method);

      if (sce->world) {
        /* Only use lighting from the light. */
        sce->world = ED_preview_prepare_world_simple(pr_main);
        const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        ED_preview_world_simple_set_rgb(sce->world, black);
      }

      BKE_view_layer_synced_ensure(*pr_main, sce, view_layer);
      for (Base &base : *BKE_view_layer_object_bases_get(view_layer)) {
        if (base.object->id.name[2] == 'p') {
          if (base.object->type == OB_LAMP) {
            base.object->data = id_cast<ID *>(la);
          }
        }
      }
    }
    else if (id_type == ID_WO) {
      World *wrld = nullptr, *origwrld = id_cast<World *>(id);

      if (origwrld) {
        BLI_assert(sp->id_copy != nullptr);
        wrld = sp->worldcopy = id_cast<World *>(sp->id_copy);
        sp->id_copy = nullptr;
        BLI_addtail(&pr_main->worlds, wrld);
      }

      ED_preview_set_visibility(pr_main, sce, view_layer, MA_SKY, sp->pr_method);
      sce->world = wrld;
    }

    return sce;
  }

  return nullptr;
}

/**
 * Fill the preview area with a solid opaque background, so transparent pixels in the render buffer
 * (unfinished tiles, areas outside the crop region) don't bleed through to the UI behind the
 * widget.
 */
static void ed_preview_draw_background(float x, float y, float w, float h)
{
  /* Matches the default render preview background, avoiding UI theme dependencies. */
  const float col[4] = {0.125f, 0.125f, 0.125f, 1.0f};

  GPU_blend(GPU_BLEND_NONE);
  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4fv(col);
  immRectf(pos, x, y, x + w, y + h);
  immUnbindProgram();
}

/* -------------------------------------------------------------------- */
/** \name GPU Texture Cache Management
 * \{ */

static void preview_gpu_texture_cache_free(uiPreview *ui_preview)
{
  if (ui_preview && ui_preview->cached_gpu_texture) {
    GPU_texture_free(static_cast<gpu::Texture *>(ui_preview->cached_gpu_texture));
    ui_preview->cached_gpu_texture = nullptr;
  }
}

/**
 * Create or update the GPU texture cache from `ibuf`.
 * \return true if the GPU texture was created/updated successfully.
 */
static bool preview_gpu_texture_cache_update(uiPreview *ui_preview, ImBuf *ibuf)
{
  if (!ui_preview || !ibuf) {
    return false;
  }

  /* Free the old GPU texture if the size changed. */
  if (ui_preview->cached_gpu_texture) {
    gpu::Texture *old_tex = static_cast<gpu::Texture *>(ui_preview->cached_gpu_texture);
    if (GPU_texture_width(old_tex) != ibuf->x || GPU_texture_height(old_tex) != ibuf->y) {
      GPU_texture_free(old_tex);
      ui_preview->cached_gpu_texture = nullptr;
    }
  }

  gpu::TextureFormat format;
  eGPUDataFormat data_format;
  const void *data_ptr;
  if (ibuf->float_data()) {
    format = gpu::TextureFormat::SFLOAT_32_32_32_32;
    data_format = GPU_DATA_FLOAT;
    data_ptr = ibuf->float_data();
  }
  else if (ibuf->byte_data()) {
    format = gpu::TextureFormat::UNORM_8_8_8_8;
    data_format = GPU_DATA_UBYTE;
    data_ptr = ibuf->byte_data();
  }
  else {
    return false;
  }

  if (!ui_preview->cached_gpu_texture) {
    gpu::Texture *gpu_tex = GPU_texture_create_2d(
        "preview_cache", ibuf->x, ibuf->y, 1, format, GPU_TEXTURE_USAGE_SHADER_READ, nullptr);
    if (!gpu_tex) {
      return false;
    }
    /* Filtering keeps the cache usable while the preview is being resized. */
    GPU_texture_filter_mode(gpu_tex, true);
    GPU_texture_extend_mode(gpu_tex, GPU_SAMPLER_EXTEND_MODE_EXTEND);
    ui_preview->cached_gpu_texture = gpu_tex;
  }

  GPU_texture_update(static_cast<gpu::Texture *>(ui_preview->cached_gpu_texture),
                     data_format,
                     data_ptr);
  return true;
}

/**
 * Draw the GPU texture cache directly, avoiding the texture re-upload #ED_draw_imbuf does on every
 * frame. Falls back to #ED_draw_imbuf when the color management shader can't be set up.
 */
static void preview_gpu_texture_cache_draw(uiPreview *ui_preview,
                                           float x,
                                           float y,
                                           float zoomx,
                                           float zoomy,
                                           const ColorManagedViewSettings *view_settings,
                                           const ColorManagedDisplaySettings *display_settings)
{
  if (!ui_preview || !ui_preview->cached_gpu_texture || !ui_preview->cached_ibuf) {
    return;
  }

  gpu::Texture *gpu_tex = static_cast<gpu::Texture *>(ui_preview->cached_gpu_texture);
  const ImBuf *ibuf = ui_preview->cached_ibuf;

  GPUVertFormat *vert_format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(vert_format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  const uint texco = GPU_vertformat_attr_add(
      vert_format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);

  const ColorSpace *colorspace = ibuf->float_data() ? ibuf->float_buffer.colorspace :
                                                      ibuf->byte_buffer.colorspace;
  const bool predivide = ibuf->float_data() != nullptr;
  /* This binds the OCIO shader used to draw the quad below. */
  if (!IMB_colormanagement_setup_glsl_draw_from_space(
          view_settings, display_settings, colorspace, ibuf->dither, predivide, false))
  {
    ED_draw_imbuf(ibuf, x, y, true, view_settings, display_settings, zoomx, zoomy);
    return;
  }

  GPU_texture_bind(gpu_tex, 0);

  const float width = ibuf->x * zoomx;
  const float height = ibuf->y * zoomy;

  immBegin(GPU_PRIM_TRI_FAN, 4);
  immAttr2f(texco, 0.0f, 0.0f);
  immVertex2f(pos, x, y);

  immAttr2f(texco, 1.0f, 0.0f);
  immVertex2f(pos, x + width, y);

  immAttr2f(texco, 1.0f, 1.0f);
  immVertex2f(pos, x + width, y + height);

  immAttr2f(texco, 0.0f, 1.0f);
  immVertex2f(pos, x, y + height);
  immEnd();

  GPU_texture_unbind(gpu_tex);
  IMB_colormanagement_finish_glsl_draw();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Preview Render Cache
 *
 * Keeps the last completed render around, so re-renders (parameter tweaks, resizing) can display
 * the previous result instead of partially rendered tiles.
 * \{ */

static bool preview_cache_is_valid(const uiPreview *ui_preview)
{
  return ui_preview && ui_preview->cached_ibuf && ui_preview->cached_width > 0 &&
         ui_preview->cached_height > 0;
}

/**
 * Whether the cached image should be shown instead of the live render result. The cache is shown
 * for the whole duration of a render, so partially rendered tiles never become visible.
 */
static bool preview_use_cached_image(const uiPreview *ui_preview)
{
  return preview_cache_is_valid(ui_preview) &&
         ui_preview->current_render_id != ui_preview->cached_render_id;
}

static void preview_cache_clear(uiPreview *ui_preview)
{
  if (!ui_preview) {
    return;
  }
  if (ui_preview->cached_ibuf) {
    IMB_freeImBuf(ui_preview->cached_ibuf);
    ui_preview->cached_ibuf = nullptr;
  }
  preview_gpu_texture_cache_free(ui_preview);
  ui_preview->cached_width = 0;
  ui_preview->cached_height = 0;
}

/** Store `ibuf` as the cached result of the render currently identified by `current_render_id`. */
static void preview_cache_update(uiPreview *ui_preview, ImBuf *ibuf, int width, int height)
{
  IMB_ensure_host_buffer(ibuf);

  /* Reuse the existing buffer when the size matches, to avoid a reallocation per render. */
  if (ui_preview->cached_ibuf && ui_preview->cached_ibuf->x == ibuf->x &&
      ui_preview->cached_ibuf->y == ibuf->y)
  {
    const size_t pixel_num = size_t(ibuf->x) * ibuf->y;
    if (ibuf->byte_data() && ui_preview->cached_ibuf->byte_data()) {
      memcpy(ui_preview->cached_ibuf->byte_data_for_write(), ibuf->byte_data(), 4 * pixel_num);
    }
    if (ibuf->float_data() && ui_preview->cached_ibuf->float_data()) {
      memcpy(ui_preview->cached_ibuf->float_data_for_write(),
             ibuf->float_data(),
             sizeof(float) * 4 * pixel_num);
    }
  }
  else {
    preview_cache_clear(ui_preview);
    ui_preview->cached_ibuf = IMB_dupImBuf(ibuf);
  }

  ui_preview->cached_width = width;
  ui_preview->cached_height = height;
  ui_preview->cached_render_id = ui_preview->current_render_id;

  preview_gpu_texture_cache_update(ui_preview, ui_preview->cached_ibuf);
}

/** \} */

/**
 * Fit a `src_width` x `src_height` image into the `dst_width` x `dst_height` widget area without
 * distorting its aspect ratio, centering it over the background fill. Scaling both axes
 * independently would stretch the intermediate result whenever the widget was resized along a
 * single axis, until the re-render at the new size completes.
 *
 * \param exact_size: the source already matches the widget, draw it pixel-exact.
 * \param r_zoom: uniform zoom factor to draw the source with.
 * \param r_ofs_x: horizontal offset from the widget origin to center the result.
 * \param r_ofs_y: vertical offset from the widget origin to center the result.
 */
static void preview_fit_centered(const int src_width,
                                 const int src_height,
                                 const int dst_width,
                                 const int dst_height,
                                 const bool exact_size,
                                 float *r_zoom,
                                 float *r_ofs_x,
                                 float *r_ofs_y)
{
  *r_zoom = exact_size ? 1.0f :
                         min_ff(float(dst_width) / float(src_width),
                                float(dst_height) / float(src_height));
  *r_ofs_x = floorf((float(dst_width) - float(src_width) * *r_zoom) * 0.5f);
  *r_ofs_y = floorf((float(dst_height) - float(src_height) * *r_zoom) * 0.5f);
}

/* new UI convention: draw is in pixel space already. */
/* uses ButtonType::Roundbox button in block to get the rect */
static bool ed_preview_draw_rect(Scene *scene,
                                 const void *owner,
                                 int split,
                                 int first,
                                 const rcti *rect,
                                 rcti *newrect,
                                 uiPreview *ui_preview,
                                 bool is_job_running,
                                 bool *r_size_matches)
{
  Render *re;
  RenderView *rv;
  RenderResult rres;
  int offx = 0;
  int newx = BLI_rcti_size_x(rect);
  int newy = BLI_rcti_size_y(rect);
  const void *split_owner = (!split || first) ? owner :
                                                static_cast<char *>(const_cast<void *>(owner)) + 1;
  bool ok = false;

  *r_size_matches = false;

  if (split) {
    if (first) {
      offx = 0;
      newx = newx / 2;
    }
    else {
      offx = newx / 2;
      newx = newx - newx / 2;
    }
  }

  /* test if something rendered ok */
  re = RE_GetRender(split_owner);

  if (re) {
    RE_AcquireResultImageViews(re, &rres);

    if (!rres.views.is_empty()) {
      /* material preview only needs monoscopy (view 0) */
      rv = RE_RenderViewGetById(&rres, 0);
    }
    else {
      /* possible the job clears the views but we're still drawing #45496 */
      rv = nullptr;
    }

    /* Draw from the live render buffer whenever it is available, even while the job is still
     * running. The background fill hides unfinished transparent tiles, so the user sees render
     * progress without artifacts. Caching is deferred until the job completes. */
    if (rv && rv->ibuf && rres.rectx > 0 && rres.recty > 0 && newx > 0 && newy > 0) {
      /* Draw at the render resolution when it matches the widget, otherwise let the GPU scale the
       * result (aspect preserved, centered) instead of dropping the frame while the new size is
       * being rendered. */
      const bool exact_size = (abs(rres.rectx - newx) < 2 && abs(rres.recty - newy) < 2);
      *r_size_matches = exact_size;

      newrect->xmax = max_ii(newrect->xmax, rect->xmin + newx + offx);
      newrect->ymax = max_ii(newrect->ymax, rect->ymin + newy);

      const float fx = rect->xmin + offx;
      const float fy = rect->ymin;

      ed_preview_draw_background(fx, fy, newx, newy);

      GPU_blend(GPU_BLEND_ALPHA_PREMULT);
      float zoom, ofs_x, ofs_y;
      if (preview_use_cached_image(ui_preview)) {
        preview_fit_centered(ui_preview->cached_width,
                             ui_preview->cached_height,
                             newx,
                             newy,
                             exact_size && ui_preview->cached_width == rres.rectx &&
                                 ui_preview->cached_height == rres.recty,
                             &zoom,
                             &ofs_x,
                             &ofs_y);
        preview_gpu_texture_cache_draw(ui_preview,
                                       fx + ofs_x,
                                       fy + ofs_y,
                                       zoom,
                                       zoom,
                                       &scene->view_settings,
                                       &scene->display_settings);
      }
      else {
        preview_fit_centered(
            rres.rectx, rres.recty, newx, newy, exact_size, &zoom, &ofs_x, &ofs_y);
        ED_draw_imbuf(rv->ibuf,
                      fx + ofs_x,
                      fy + ofs_y,
                      true,
                      &scene->view_settings,
                      &scene->display_settings,
                      zoom,
                      zoom);
      }
      GPU_blend(GPU_BLEND_NONE);

      ok = true;

      /* Clear the flag after drawing but before caching, so a render started in a previous frame
       * can update the cache in this one. */
      const bool render_started_this_frame = ui_preview &&
                                             (ui_preview->tag &
                                              UI_PREVIEW_TAG_RENDER_IN_PROGRESS);
      if (render_started_this_frame) {
        ui_preview->tag &= ~UI_PREVIEW_TAG_RENDER_IN_PROGRESS;
      }

      /* Only cache once the job has fully completed and the cache is stale. Caching while
       * #UI_PREVIEW_TAG_RENDER_IN_PROGRESS is set would store incomplete data. */
      if (!is_job_running && ui_preview && !render_started_this_frame &&
          ui_preview->current_render_id != ui_preview->cached_render_id)
      {
        preview_cache_update(ui_preview, rv->ibuf, rres.rectx, rres.recty);
      }
    }
  }

  if (!ok && preview_cache_is_valid(ui_preview) && newx > 0 && newy > 0) {
    /* No current render result, possibly because a new render cleared the views. Fall back to the
     * cached result, scaled to the requested size. */
    newrect->xmax = max_ii(newrect->xmax, rect->xmin + newx + offx);
    newrect->ymax = max_ii(newrect->ymax, rect->ymin + newy);

    const float fx = rect->xmin + offx;
    const float fy = rect->ymin;

    float zoom, ofs_x, ofs_y;
    preview_fit_centered(ui_preview->cached_width,
                         ui_preview->cached_height,
                         newx,
                         newy,
                         false,
                         &zoom,
                         &ofs_x,
                         &ofs_y);

    ed_preview_draw_background(fx, fy, newx, newy);
    GPU_blend(GPU_BLEND_ALPHA_PREMULT);
    preview_gpu_texture_cache_draw(ui_preview,
                                   fx + ofs_x,
                                   fy + ofs_y,
                                   zoom,
                                   zoom,
                                   &scene->view_settings,
                                   &scene->display_settings);
    GPU_blend(GPU_BLEND_NONE);

    ok = true;
  }

  if (re) {
    RE_ReleaseResultImageViews(re, &rres);
  }

  return ok;
}

void ED_preview_draw(
    const bContext *C, void *idp, void *parentp, void *slotp, uiPreview *ui_preview, rcti *rect)
{
  if (idp) {
    Scene *scene = CTX_data_scene(C);
    wmWindowManager *wm = CTX_wm_manager(C);
    ID *id = static_cast<ID *>(idp);
    ID *parent = static_cast<ID *>(parentp);
    MTex *slot = static_cast<MTex *>(slotp);
    SpaceProperties *sbuts = CTX_wm_space_properties(C);
    const void *owner = CTX_wm_area(C);
    ShaderPreview *sp = static_cast<ShaderPreview *>(
        WM_jobs_customdata_from_type(wm, owner, WM_JOB_TYPE_RENDER_PREVIEW));
    rcti newrect;
    bool ok;
    int newx = BLI_rcti_size_x(rect);
    int newy = BLI_rcti_size_y(rect);

    newrect.xmin = rect->xmin;
    newrect.xmax = rect->xmin;
    newrect.ymin = rect->ymin;
    newrect.ymax = rect->ymin;

    const bool is_job_running = WM_jobs_test(wm, owner, WM_JOB_TYPE_RENDER_PREVIEW);

    bool size_matches;
    if (parent) {
      bool size_matches_second;
      ok = ed_preview_draw_rect(
          scene, owner, 1, 1, rect, &newrect, ui_preview, is_job_running, &size_matches);
      ok &= ed_preview_draw_rect(
          scene, owner, 1, 0, rect, &newrect, ui_preview, is_job_running, &size_matches_second);
      size_matches &= size_matches_second;
    }
    else {
      ok = ed_preview_draw_rect(
          scene, owner, 0, 0, rect, &newrect, ui_preview, is_job_running, &size_matches);
    }

    if (ok) {
      *rect = newrect;
    }

    /* Start a new preview render job if signaled through `sbuts->preview`, if no render result was
     * found and no preview render job is running, or if the size of the preview changed. */

    /* The result on screen was rendered for a different widget size, e.g. the preview was resized
     * with the grip. It is drawn scaled to fit meanwhile, but nothing else would ever trigger the
     * re-render that makes it sharp again, since the finished job is gone from the job list. */
    const bool stale_size = (!is_job_running && !size_matches);
    const bool trigger_size_mismatch =
        stale_size || (sp && (abs(sp->sizex - newx) >= 2 || abs(sp->sizey - newy) > 2));

    if ((sbuts != nullptr && sbuts->preview) || (ui_preview->tag & UI_PREVIEW_TAG_DIRTY) ||
        (!ok && !is_job_running) || trigger_size_mismatch)
    {
      if (sbuts != nullptr) {
        sbuts->preview = 0;
      }

      ED_preview_shader_job(C, owner, id, parent, slot, newx, newy, PR_BUTS_RENDER);
      ui_preview->tag &= ~UI_PREVIEW_TAG_DIRTY;
      /* Let drawing know a new render is in progress, so it keeps showing the cache. */
      ui_preview->current_render_id++;
      if (ui_preview->cached_ibuf) {
        ui_preview->tag |= UI_PREVIEW_TAG_RENDER_IN_PROGRESS;
      }
    }
  }
}

void ED_previews_tag_dirty_by_id(const Main &bmain, const ID &id)
{
  for (const bScreen &screen : bmain.screens) {
    for (const ScrArea &area : screen.areabase) {
      for (const ARegion &region : area.regionbase) {
        for (uiPreview &preview : region.ui_previews) {
          if (preview.id_session_uid == id.session_uid) {
            preview.tag |= UI_PREVIEW_TAG_DIRTY;
          }
        }
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Preview
 * \{ */

struct ObjectPreviewData {
  /* The main for the preview, not of the current file. */
  Main *pr_main;
  /* Copy of the object to create the preview for. The copy is for thread safety (and to insert
   * it into its own main). */
  Object *object;
  /* Current frame. */
  int cfra;
  int sizex;
  int sizey;
};

static bool object_preview_is_type_supported(const Object *ob)
{
  return OB_TYPE_IS_GEOMETRY(ob->type);
}

static Object *object_preview_camera_create(Main *preview_main,
                                            Scene *scene,
                                            ViewLayer *view_layer,
                                            Object *preview_object)
{
  Object *camera = BKE_object_add(preview_main, scene, view_layer, OB_CAMERA, "Preview Camera");

  float rotmat[3][3];
  float dummy_scale[3];
  mat4_to_loc_rot_size(camera->loc, rotmat, dummy_scale, preview_object->object_to_world().ptr());

  /* Camera is Y up, so needs additional rotations to obliquely face the front. */
  float drotmat[3][3];
  const float eul[3] = {M_PI * 0.4f, 0.0f, M_PI * 0.1f};
  eul_to_mat3(drotmat, eul);
  mul_m3_m3_post(rotmat, drotmat);

  camera->rotmode = ROT_MODE_QUAT;
  mat3_to_quat(camera->quat, rotmat);

  /* Nice focal length for close portraiture. */
  (id_cast<Camera *>(camera->data))->lens = 85;

  return camera;
}

static Scene *object_preview_scene_create(const ObjectPreviewData *preview_data,
                                          Depsgraph **r_depsgraph)
{
  Scene *scene = BKE_scene_add(preview_data->pr_main, "Object preview scene");
  /* Preview need to be in the current frame to get a thumbnail similar of what
   * viewport displays. */
  scene->r.cfra = preview_data->cfra;

  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  Depsgraph *depsgraph = DEG_graph_new(
      preview_data->pr_main, scene, view_layer, DAG_EVAL_VIEWPORT);

  BLI_assert(preview_data->object != nullptr);
  BLI_addtail(&preview_data->pr_main->objects, preview_data->object);

  BKE_collection_object_add(preview_data->pr_main, scene->master_collection, preview_data->object);

  Object *camera_object = object_preview_camera_create(
      preview_data->pr_main, scene, view_layer, preview_data->object);

  scene->camera = camera_object;
  scene->r.xsch = preview_data->sizex;
  scene->r.ysch = preview_data->sizey;
  scene->r.size = 100;

  BKE_view_layer_synced_ensure(*preview_data->pr_main, scene, view_layer);
  Base *preview_base = BKE_view_layer_base_find(view_layer, preview_data->object);
  /* For 'view selected' below. */
  preview_base->flag |= BASE_SELECTED;

  DEG_graph_build_from_view_layer(depsgraph);
  DEG_evaluate_on_refresh(depsgraph);

  ED_view3d_camera_to_view_selected_with_set_clipping(
      preview_data->pr_main, depsgraph, scene, camera_object);

  BKE_scene_graph_update_tagged(depsgraph, preview_data->pr_main);

  *r_depsgraph = depsgraph;
  return scene;
}

static void object_preview_render(const PreviewImage *prv_img,
                                  IconPreview *preview,
                                  const eIconSizes icon_size)
{
  Main *preview_main = BKE_main_new();
  char err_out[256] = "unknown";

  BLI_assert(preview->id_copy && (preview->id_copy != preview->id));

  ObjectPreviewData preview_data = {};
  preview_data.pr_main = preview_main;
  /* Act on a copy. */
  preview_data.object = id_cast<Object *>(preview->id_copy);
  preview_data.cfra = preview->scene->r.cfra;
  preview_data.sizex = prv_img->w[icon_size];
  preview_data.sizey = prv_img->h[icon_size];

  Depsgraph *depsgraph;
  Scene *scene = object_preview_scene_create(&preview_data, &depsgraph);

  /* Ownership is now ours. */
  preview->id_copy = nullptr;

  View3DShading shading;
  BKE_screen_view3d_shading_init(&shading);
  /* Enable shadows, makes it a bit easier to see the shape. */
  shading.flag |= V3D_SHADING_SHADOW;

  ImBuf *ibuf = ED_view3d_draw_offscreen_imbuf_simple(depsgraph,
                                                      DEG_get_evaluated_scene(depsgraph),
                                                      &shading,
                                                      OB_TEXTURE,
                                                      DEG_get_evaluated(depsgraph, scene->camera),
                                                      prv_img->w[icon_size],
                                                      prv_img->h[icon_size],
                                                      ImBufFlags::ByteData,
                                                      V3D_OFSDRAW_OVERRIDE_SCENE_SETTINGS,
                                                      R_ALPHAPREMUL,
                                                      nullptr,
                                                      nullptr,
                                                      nullptr,
                                                      err_out);
  /* TODO: color-management? */

  if (ibuf) {
    icon_copy_rect(ibuf, prv_img->w[icon_size], prv_img->h[icon_size], prv_img->rect[icon_size]);
    IMB_freeImBuf(ibuf);
  }

  DEG_graph_free(depsgraph);
  BKE_main_free(preview_main);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Collection Preview
 *
 * For the most part this reuses the object preview code by creating an instance collection empty
 * object and rendering that.
 *
 * \{ */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Action Preview
 * \{ */

static PoseBackup *action_preview_render_prepare(IconPreview *preview)
{
  Object *object = preview->active_object;
  if (object == nullptr) {
    WM_global_report(RPT_WARNING, "No active object, unable to apply the Action before rendering");
    return nullptr;
  }
  if (object->pose == nullptr) {
    WM_global_reportf(RPT_WARNING,
                      "Object %s has no pose, unable to apply the Action before rendering",
                      object->id.name + 2);
    return nullptr;
  }

  /* Create a backup of the current pose. */
  animrig::Action &pose_action = reinterpret_cast<bAction *>(preview->id)->wrap();

  if (pose_action.slot_array_num == 0) {
    WM_global_report(RPT_WARNING, "Action has no data, cannot render preview");
    return nullptr;
  }

  animrig::Slot &slot = animrig::get_best_pose_slot_for_id(object->id, pose_action);
  PoseBackup *pose_backup = BKE_pose_backup_create_all_bones({object}, &pose_action);

  /* Apply the Action as pose, so that it can be rendered. This assumes the Action represents a
   * single pose, and that thus the evaluation time doesn't matter. */
  AnimationEvalContext anim_eval_context = {preview->depsgraph, 0.0f};
  animrig::pose_apply_action_all_bones(object, &pose_action, slot.handle, &anim_eval_context);

  /* Force evaluation of the new pose, before the preview is rendered. */
  DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  DEG_evaluate_on_refresh(preview->depsgraph);

  return pose_backup;
}

static void action_preview_render_cleanup(IconPreview *preview, PoseBackup *pose_backup)
{
  if (pose_backup == nullptr) {
    return;
  }
  BKE_pose_backup_restore(pose_backup);
  BKE_pose_backup_free(pose_backup);

  DEG_id_tag_update(&preview->active_object->id, ID_RECALC_GEOMETRY);
}

/* Render a pose from the scene camera. It is assumed that the scene camera is
 * capturing the pose. The pose is applied temporarily to the current object
 * before rendering. */
static void action_preview_render(const PreviewImage *prv_img,
                                  IconPreview *preview,
                                  const eIconSizes icon_size)
{
  char err_out[256] = "";

  Depsgraph *depsgraph = preview->depsgraph;
  /* Not all code paths that lead to this function actually provide a depsgraph.
   * The "Refresh Asset Preview" button (#ED_OT_lib_id_generate_preview) does,
   * but #WM_OT_previews_ensure does not. */
  BLI_assert(depsgraph != nullptr);
  BLI_assert(preview->scene == DEG_get_input_scene(depsgraph));

  /* Apply the pose before getting the evaluated scene, so that the new pose is evaluated. */
  PoseBackup *pose_backup = action_preview_render_prepare(preview);

  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Object *camera_eval = scene_eval->camera;
  if (camera_eval == nullptr) {
    printf("Scene has no camera, unable to render preview of %s without it.\n",
           preview->id->name + 2);
    action_preview_render_cleanup(preview, pose_backup);
    return;
  }

  /* This renders with the Workbench engine settings stored on the Scene. */
  ImBuf *ibuf = ED_view3d_draw_offscreen_imbuf_simple(depsgraph,
                                                      scene_eval,
                                                      nullptr,
                                                      OB_SOLID,
                                                      camera_eval,
                                                      prv_img->w[icon_size],
                                                      prv_img->h[icon_size],
                                                      ImBufFlags::ByteData,
                                                      V3D_OFSDRAW_NONE,
                                                      R_ADDSKY,
                                                      nullptr,
                                                      nullptr,
                                                      nullptr,
                                                      err_out);

  action_preview_render_cleanup(preview, pose_backup);

  if (err_out[0] != '\0') {
    printf("Error rendering Action %s preview: %s\n", preview->id->name + 2, err_out);
  }

  if (ibuf) {
    icon_copy_rect(ibuf, prv_img->w[icon_size], prv_img->h[icon_size], prv_img->rect[icon_size]);
    IMB_freeImBuf(ibuf);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Scene Preview
 * \{ */

static bool scene_preview_is_supported(const Scene *scene)
{
  return scene->camera != nullptr;
}

static void scene_preview_render(const PreviewImage *prv_img,
                                 IconPreview *preview,
                                 const eIconSizes icon_size,
                                 ReportList *reports)
{
  Depsgraph *depsgraph = preview->depsgraph;
  /* Not all code paths that lead to this function actually provide a depsgraph.
   * The "Refresh Asset Preview" button (#ED_OT_lib_id_generate_preview) does,
   * but #WM_OT_previews_ensure does not. */
  BLI_assert(depsgraph != nullptr);
  BLI_assert(preview->id != nullptr);

  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Object *camera_eval = scene_eval->camera;
  if (camera_eval == nullptr) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Scene has no camera, unable to render preview of %s without it.",
                BKE_id_name(*preview->id));
    return;
  }

  char err_out[256] = "";
  /* This renders with the Workbench engine settings stored on the Scene. */
  ImBuf *ibuf = ED_view3d_draw_offscreen_imbuf_simple(depsgraph,
                                                      scene_eval,
                                                      nullptr,
                                                      OB_SOLID,
                                                      camera_eval,
                                                      prv_img->w[icon_size],
                                                      prv_img->h[icon_size],
                                                      ImBufFlags::ByteData,
                                                      V3D_OFSDRAW_NONE,
                                                      R_ADDSKY,
                                                      nullptr,
                                                      nullptr,
                                                      nullptr,
                                                      err_out);

  if (err_out[0] != '\0') {
    BKE_reportf(reports,
                RPT_ERROR,
                "Error rendering Scene %s preview: %s.",
                BKE_id_name(*preview->id),
                err_out);
  }

  if (ibuf) {
    icon_copy_rect(ibuf, prv_img->w[icon_size], prv_img->h[icon_size], prv_img->rect[icon_size]);
    IMB_freeImBuf(ibuf);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name New Shader Preview System
 * \{ */

/* inside thread, called by renderer, sets job update value */
static void shader_preview_update(void *spv, RenderResult * /*rr*/, rcti * /*rect*/)
{
  ShaderPreview *sp = static_cast<ShaderPreview *>(spv);

  *(sp->do_update) = true;
}

/* called by renderer, checks job value */
static bool shader_preview_break(void *spv)
{
  ShaderPreview *sp = static_cast<ShaderPreview *>(spv);

  return *(sp->stop);
}

static void shader_preview_updatejob(void * /*spv*/) {}

/**
 * No notifier is sent when the job ends on purpose: #NC_MATERIAL sets `sbuts->preview` in
 * #space_buttons.cc, which restarts the job immediately and makes the preview flicker. The UI
 * redraws on the next frame anyway.
 */
static void shader_preview_endjob(void * /*spv*/) {}

/** Shared state for the multi-threaded texture preview evaluation. */
struct TexturePreviewData {
  ShaderPreview *sp;
  Tex *tex;
  float *rect_float;
  int width;
  /**
   * Size in pixels the `[-1, 1]` texture coordinate range is mapped over. Normally the preview
   * height on both axes (square footprint), reduced on one axis for non-square image textures so
   * the image keeps its aspect ratio.
   */
  float span_x;
  float span_y;
  bool cancelled;
};

static void shader_preview_texture_row_task(void *__restrict userdata,
                                            const int y,
                                            const TaskParallelTLS *__restrict tls)
{
  TexturePreviewData *data = static_cast<TexturePreviewData *>(userdata);

  if (data->cancelled) {
    return;
  }

  ImagePool *thread_pool = *static_cast<ImagePool **>(tls->userdata_chunk);
  const int width = data->width;
  float *rect_float = data->rect_float + (size_t(y) * width * 4);

  /* Tex coords between -1.0f and 1.0f. */
  float tex_coord[3] = {0.0f, 0.0f, 0.0f};
  tex_coord[1] = (float(y) / data->span_y) * 2.0f - 1.0f;

  for (int x = 0; x < width; x++) {
    tex_coord[0] = (float(x) / data->span_x) * 2.0f - 1.0f;

    /* Evaluate texture at tex_coord. */
    TexResult texres = {0};
    BKE_texture_get_value_ex(data->tex, tex_coord, &texres, thread_pool, true);
    copy_v4_fl4(rect_float,
                texres.trgba[0],
                texres.trgba[1],
                texres.trgba[2],
                texres.talpha ? texres.trgba[3] : 1.0f);

    rect_float += 4;
  }

  /* Checking every row would dominate the cost of a small preview. */
  if ((y % 8) == 0 && shader_preview_break(data->sp)) {
    data->cancelled = true;
  }
}

static void shader_preview_texture_init_func(const void *__restrict userdata,
                                             void *__restrict chunk)
{
  const TexturePreviewData *data = static_cast<const TexturePreviewData *>(userdata);
  ImagePool **thread_pool = static_cast<ImagePool **>(chunk);

  *thread_pool = BKE_image_pool_new();
  BKE_texture_fetch_images_for_pool(data->tex, *thread_pool);
}

static void shader_preview_texture_free_func(const void *__restrict /*userdata*/,
                                             void *__restrict chunk)
{
  ImagePool **thread_pool = static_cast<ImagePool **>(chunk);
  if (*thread_pool) {
    BKE_image_pool_free(*thread_pool);
    *thread_pool = nullptr;
  }
}

/* Renders texture directly to render buffer, using all threads. */
static void shader_preview_texture(ShaderPreview *sp, Tex *tex, Scene *sce, Render *re)
{
  /* Setup output buffer. */
  int width = sp->sizex;
  int height = sp->sizey;

  /* This is needed otherwise no RenderResult is created. */
  sce->r.scemode &= ~R_BUTS_PREVIEW;
  RE_InitState(re, nullptr, &sce->r, &sce->view_layers, nullptr, width, height, nullptr);
  RE_SetScene(re, sce);

  /* Create buffer in empty RenderView created in the init step. */
  const size_t buffer_size = size_t(4) * width * height;
  RenderResult *rr = RE_AcquireResultWrite(re);
  RenderView *rv = static_cast<RenderView *>(rr->views.first);
  ImBuf *rv_ibuf = RE_RenderViewEnsureImBuf(rr, rv);
  rv_ibuf->assign_float_data(MEM_new_array_zeroed<float>(buffer_size, __func__));
  RE_ReleaseResult(re);

  /* Fill a private buffer rather than the render result: the UI draws the render result while this
   * runs, so filling it row by row exposes half-written scan-lines, and a cancelled fill leaves
   * holes the preview cache can't tell apart from a finished render. */
  float *fill_buffer = MEM_new_array_zeroed<float>(buffer_size, __func__);

  /* Fill in image buffer, one row per task. */
  TexturePreviewData data;
  data.sp = sp;
  data.tex = tex;
  data.rect_float = fill_buffer;
  data.width = width;
  data.cancelled = false;

  /* The texture coordinate range is mapped over a square of the preview height, so procedural
   * textures aren't distorted by the widget being wider than it is tall. Image textures map that
   * square onto the whole image regardless of its resolution, which stretches non-square images.
   * Shrink the coordinate span on the longer axis so the image is fitted into the square instead. */
  data.span_x = float(height);
  data.span_y = float(height);
  if (tex->type == TEX_IMAGE && tex->ima) {
    int image_size[2];
    BKE_image_get_size(tex->ima, &tex->iuser, &image_size[0], &image_size[1]);
    if (image_size[0] > 0 && image_size[1] > 0) {
      const float aspect = float(image_size[0]) / float(image_size[1]);
      if (aspect > 1.0f) {
        data.span_y = float(height) / aspect;
      }
      else {
        data.span_x = float(height) * aspect;
      }
    }
  }

  TaskParallelSettings settings;
  BLI_parallel_range_settings_defaults(&settings);

  /* Texture evaluation is not thread-safe with a shared pool, so give each thread its own. */
  ImagePool *pool_chunk = nullptr;
  settings.userdata_chunk = &pool_chunk;
  settings.userdata_chunk_size = sizeof(ImagePool *);
  settings.func_init = shader_preview_texture_init_func;
  settings.func_free = shader_preview_texture_free_func;
  settings.use_threading = (height > 64);
  settings.min_iter_per_thread = 4;

  BLI_task_parallel_range(0, height, &data, shader_preview_texture_row_task, &settings);

  /* Publish in a single step, and only when every row was evaluated. A cancelled fill is discarded
   * so no partial image can reach the screen or the cache. */
  if (data.cancelled || shader_preview_break(sp)) {
    MEM_delete(fill_buffer);
    return;
  }

  rr = RE_AcquireResultWrite(re);
  rv = static_cast<RenderView *>(rr->views.first);
  rv_ibuf = RE_RenderViewEnsureImBuf(rr, rv);
  rv_ibuf->assign_float_data(fill_buffer);
  RE_ReleaseResult(re);
}

static void shader_preview_render(ShaderPreview *sp, ID *id, int split, int first)
{
  Render *re;
  Scene *sce;
  float oldlens;
  short idtype = GS(id->name);
  int sizex;
  Main *pr_main = sp->pr_main;

  /* in case of split preview, use border render */
  if (split) {
    if (first) {
      sizex = sp->sizex / 2;
    }
    else {
      sizex = sp->sizex - sp->sizex / 2;
    }
  }
  else {
    sizex = sp->sizex;
  }

  /* we have to set preview variables first */
  sce = preview_get_scene(pr_main);
  if (sce) {
    sce->r.xsch = sizex;
    sce->r.ysch = sp->sizey;
    sce->r.size = 100;
  }

  /* get the stuff from the builtin preview dbase */
  sce = preview_prepare_scene(sp->bmain, sp->scene, id, idtype, sp);
  if (sce == nullptr) {
    return;
  }

  const void *split_owner = (!split || first) ?
                                sp->owner :
                                static_cast<char *>(const_cast<void *>(sp->owner)) + 1;
  re = RE_GetRender(split_owner);

  /* full refreshed render from first tile */
  if (re == nullptr) {
    re = RE_NewRender(split_owner);
  }

  /* sce->r gets copied in RE_InitState! */
  sce->r.scemode &= ~(R_MATNODE_PREVIEW | R_TEXNODE_PREVIEW);
  sce->r.scemode &= ~R_NO_IMAGE_LOAD;

  if (sp->pr_method == PR_ICON_RENDER) {
    sce->r.scemode |= R_NO_IMAGE_LOAD;
    sce->display.render_aa = SCE_DISPLAY_AA_SAMPLES_8;
  }
  else { /* PR_BUTS_RENDER */
    sce->display.render_aa = SCE_DISPLAY_AA_SAMPLES_8;
  }

  /* Callbacks are cleared on GetRender(). */
  if (sp->pr_method == PR_BUTS_RENDER) {
    RE_display_update_cb(re, sp, shader_preview_update);
  }
  /* set this for all previews, default is react to G.is_break still */
  RE_test_break_cb(re, sp, shader_preview_break);

  /* lens adjust */
  oldlens = (id_cast<Camera *>(sce->camera->data))->lens;
  if (sizex > sp->sizey) {
    (id_cast<Camera *>(sce->camera->data))->lens *= float(sp->sizey) / float(sizex);
  }

  /* entire cycle for render engine */
  if (idtype == ID_TE) {
    shader_preview_texture(sp, id_cast<Tex *>(id), sce, re);
  }
  else {
    /* Render preview scene */
    RE_PreviewRender(re, pr_main, sce);
  }

  (id_cast<Camera *>(sce->camera->data))->lens = oldlens;

  /* handle results */
  if (sp->pr_method == PR_ICON_RENDER) {
    if (sp->pr_rect) {
      RE_ResultGet32(re, reinterpret_cast<uint8_t *>(sp->pr_rect));
    }
  }

  /* unassign the pointers, reset vars */
  preview_prepare_scene(sp->bmain, sp->scene, nullptr, GS(id->name), sp);

  /* XXX bad exception, end-exec is not being called in render, because it uses local main. */
#if 0
  if (idtype == ID_TE) {
    Tex *tex = (Tex *)id;
    if (tex->use_nodes && tex->nodetree) {
      ntreeEndExecTree(tex->nodetree);
    }
  }
#endif
}

/* runs inside thread for material and icons */
static void shader_preview_startjob(void *customdata, bool *stop, bool *do_update)
{
  ShaderPreview *sp = static_cast<ShaderPreview *>(customdata);

  sp->stop = stop;
  sp->do_update = do_update;

  if (sp->parent) {
    shader_preview_render(sp, sp->id, 1, 1);
    shader_preview_render(sp, sp->parent, 1, 0);
  }
  else {
    shader_preview_render(sp, sp->id, 0, 0);
  }

  *do_update = true;
}

static void preview_id_copy_free(ID *id)
{
  BKE_libblock_free_datablock(id, 0);
  BKE_libblock_free_data(id, false);
  MEM_delete(id);
}

static void shader_preview_free(void *customdata)
{
  ShaderPreview *sp = static_cast<ShaderPreview *>(customdata);
  Main *pr_main = sp->pr_main;
  ID *main_id_copy = nullptr;
  ID *sub_id_copy = nullptr;

  if (sp->matcopy) {
    main_id_copy = id_cast<ID *>(sp->matcopy);
    BLI_remlink(&pr_main->materials, sp->matcopy);
  }
  if (sp->texcopy) {
    BLI_assert(main_id_copy == nullptr);
    main_id_copy = id_cast<ID *>(sp->texcopy);
    BLI_remlink(&pr_main->textures, sp->texcopy);
  }
  if (sp->worldcopy) {
    /* worldcopy is also created for material with `Preview World` enabled */
    if (main_id_copy) {
      sub_id_copy = id_cast<ID *>(sp->worldcopy);
    }
    else {
      main_id_copy = id_cast<ID *>(sp->worldcopy);
    }
    BLI_remlink(&pr_main->worlds, sp->worldcopy);
  }
  if (sp->lampcopy) {
    BLI_assert(main_id_copy == nullptr);
    main_id_copy = id_cast<ID *>(sp->lampcopy);
    BLI_remlink(&pr_main->lights, sp->lampcopy);
  }
  if (sp->own_id_copy) {
    if (sp->id_copy) {
      preview_id_copy_free(sp->id_copy);
    }
    if (main_id_copy) {
      preview_id_copy_free(main_id_copy);
    }
    if (sub_id_copy) {
      preview_id_copy_free(sub_id_copy);
    }
  }

  MEM_delete(sp);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Icon Preview
 * \{ */

static void icon_copy_rect(const ImBuf *ibuf, uint w, uint h, uint *rect)
{
  if (ibuf == nullptr || (ibuf->byte_data() == nullptr && ibuf->float_data() == nullptr) ||
      rect == nullptr)
  {
    return;
  }

  float scaledx, scaledy;
  if (ibuf->x > ibuf->y) {
    scaledx = float(w);
    scaledy = (float(ibuf->y) / float(ibuf->x)) * float(w);
  }
  else {
    scaledx = (float(ibuf->x) / float(ibuf->y)) * float(h);
    scaledy = float(h);
  }

  /* Scaling down must never assign zero width/height, see: #89868. */
  int ex = std::max<int>(1, scaledx);
  int ey = std::max<int>(1, scaledy);

  int dx = (w - ex) / 2;
  int dy = (h - ey) / 2;

  ImBuf *ima = IMB_scale_into_new(ibuf, ex, ey, IMBScaleFilter::Nearest, false);
  if (ima == nullptr) {
    return;
  }

  /* if needed, convert to 32 bits */
  if (ima->byte_data() == nullptr) {
    IMB_byte_from_float(ima);
  }

  const uint *srect = reinterpret_cast<const uint *>(ima->byte_data());
  uint *drect = rect;

  drect += dy * w + dx;
  for (; ey > 0; ey--) {
    memcpy(drect, srect, ex * sizeof(int));
    drect += w;
    srect += ima->x;
  }

  IMB_freeImBuf(ima);
}

static void set_alpha(char *cp, int sizex, int sizey, char alpha)
{
  int a, size = sizex * sizey;

  for (a = 0; a < size; a++, cp += 4) {
    cp[3] = alpha;
  }
}

static void icon_preview_startjob(void *customdata, bool *stop, bool *do_update)
{
  ShaderPreview *sp = static_cast<ShaderPreview *>(customdata);

  if (sp->pr_method == PR_ICON_DEFERRED) {
    BLI_assert_unreachable();
    return;
  }

  ID *id = sp->id;
  short idtype = GS(id->name);

  BLI_assert(id != nullptr);

  if (idtype == ID_IM) {
    Image *ima = id_cast<Image *>(id);
    ImBuf *ibuf = nullptr;
    ImageUser iuser;
    BKE_imageuser_default(&iuser);

    if (ima == nullptr) {
      return;
    }

    /* setup dummy image user */
    iuser.framenr = 1;
    iuser.scene = sp->scene;

    /* NOTE(@elubie): this needs to be changed: here image is always loaded if not
     * already there. Very expensive for large images. Need to find a way to
     * only get existing `ibuf`. */
    ibuf = BKE_image_acquire_ibuf(ima, &iuser, nullptr);
    if (ibuf == nullptr || (ibuf->byte_data() == nullptr && ibuf->float_data() == nullptr)) {
      BKE_image_release_ibuf(ima, ibuf, nullptr);
      return;
    }

    icon_copy_rect(ibuf, sp->sizex, sp->sizey, sp->pr_rect);

    *do_update = true;

    BKE_image_release_ibuf(ima, ibuf, nullptr);
  }
  else {
    /* re-use shader job */
    shader_preview_startjob(customdata, stop, do_update);

    /* world is rendered with alpha=0, so it wasn't displayed
     * this could be render option for sky to, for later */
    if (idtype == ID_WO) {
      set_alpha(reinterpret_cast<char *>(sp->pr_rect), sp->sizex, sp->sizey, 255);
    }
  }
}

/* use same function for icon & shader, so the job manager
 * does not run two of them at the same time. */

static void common_preview_startjob(void *customdata, wmJobWorkerStatus *worker_status)
{
  ShaderPreview *sp = static_cast<ShaderPreview *>(customdata);

  if (ELEM(sp->pr_method, PR_ICON_RENDER, PR_ICON_DEFERRED)) {
    icon_preview_startjob(customdata, &worker_status->stop, &worker_status->do_update);
  }
  else {
    shader_preview_startjob(customdata, &worker_status->stop, &worker_status->do_update);
  }
}

/**
 * Some ID types already have their own, more focused rendering (only objects right now). This is
 * for the other ones, which all share #ShaderPreview and some functions.
 */
static void other_id_types_preview_render(const PreviewImage *prv_img,
                                          IconPreview *ip,
                                          const eIconSizes icon_size,
                                          const ePreviewRenderMethod pr_method,
                                          wmJobWorkerStatus *worker_status)
{
  ShaderPreview *sp = MEM_new_zeroed<ShaderPreview>("Icon ShaderPreview");

  /* These types don't use the ShaderPreview mess, they have their own types and functions. */
  BLI_assert(!ip->id || !ELEM(GS(ip->id->name), ID_OB));

  /* Construct shader preview from image size and preview custom-data. */
  sp->scene = ip->scene;
  sp->owner = ip->owner;
  sp->sizex = prv_img->w[icon_size];
  sp->sizey = prv_img->h[icon_size];
  sp->pr_method = pr_method;
  sp->pr_rect = prv_img->rect[icon_size];
  sp->id = ip->id;
  sp->id_copy = ip->id_copy;
  sp->bmain = ip->bmain;
  sp->own_id_copy = false;
  Material *ma = nullptr;

  if (sp->pr_method == PR_ICON_RENDER) {
    BLI_assert(ip->id);

    /* grease pencil use its own preview file */
    if (GS(ip->id->name) == ID_MA) {
      ma = id_cast<Material *>(ip->id);
    }

    if ((ma == nullptr) || (ma->gp_style == nullptr)) {
      sp->pr_main = G.pr_main;
    }
    else {
      sp->pr_main = G_pr_main_grease_pencil;
    }
  }

  common_preview_startjob(sp, worker_status);
  shader_preview_free(sp);
}

/* exported functions */

static void icon_preview_startjob_all_sizes(void *customdata, wmJobWorkerStatus *worker_status)
{
  IconPreview *ip = static_cast<IconPreview *>(customdata);

  for (int i = 0; i < NUM_ICON_SIZES; i++) {
    PreviewImage *prv = static_cast<PreviewImage *>(ip->owner);
    const eIconSizes icon_size = eIconSizes(i);
    if (ip->render_size[icon_size] == false) {
      continue;
    }

    /* Is this a render job or a deferred loading job? */
    const ePreviewRenderMethod pr_method = (prv->runtime->deferred_loading_data) ?
                                               PR_ICON_DEFERRED :
                                               PR_ICON_RENDER;

    if (worker_status->stop) {
      break;
    }

    /* Non-thread-protected reading is not an issue here, because we are only trying
     * to avoid unnecessary work when the preview is to be deleted. */
    if (prv->runtime->tag[icon_size] & PRV_TAG_DEFERRED_DELETE) {
      continue;
    }

    /* check_engine_supports_preview() checks whether the engine supports "preview mode" (think:
     * Material Preview). This check is only relevant when the render function called below is
     * going to use such a mode. Group, Object and Action render functions use Solid mode, though,
     * so they can skip this test. Same is true for Images and Brushes, they can also skip this
     * test since their preview is just pulled from ImBuf which is not dependent on the render
     * engine. */
    /* TODO: Decouple the ID-type-specific render functions from this function, so that it's not
     * necessary to know here what happens inside lower-level functions. */
    const bool use_solid_render_mode = (ip->id != nullptr) &&
                                       ELEM(GS(ip->id->name), ID_OB, ID_AC, ID_IM, ID_GR, ID_SCE);
    if (!use_solid_render_mode && preview_method_is_render(pr_method) &&
        !ED_check_engine_supports_preview(ip->scene))
    {
      continue;
    }

    /* Workaround: Skip preview renders for linked IDs. Preview rendering can be slow and even
     * freeze the UI (e.g. on Eevee shader compilation). And since the result will never be stored
     * in a file, it's done every time the file is reloaded, so this becomes a frequent annoyance.
     */
    if (!use_solid_render_mode && ip->id && !ID_IS_EDITABLE(ip->id)) {
      continue;
    }

    BLI_assert(BKE_previewimg_is_rendering(prv, i));

    if (ip->id != nullptr) {
      switch (GS(ip->id->name)) {
        case ID_OB:
          if (object_preview_is_type_supported(id_cast<Object *>(ip->id))) {
            /* Much simpler than the ShaderPreview mess used for other ID types. */
            object_preview_render(prv, ip, icon_size);
          }
          continue;
        case ID_GR:
          BLI_assert(BKE_collection_contains_geometry_recursive(
              reinterpret_cast<const Collection *>(ip->id)));
          /* A collection instance empty was created, so this can just reuse the object preview
           * rendering. */
          object_preview_render(prv, ip, icon_size);
          continue;
        case ID_AC:
          action_preview_render(prv, ip, icon_size);
          continue;
        case ID_SCE:
          scene_preview_render(prv, ip, icon_size, worker_status->reports);
          continue;
        default:
          /* Fall through to the same code as the `ip->id == nullptr` case. */
          break;
      }
    }
    other_id_types_preview_render(prv, ip, icon_size, pr_method, worker_status);
  }
}

static void icon_preview_endjob(void *customdata, const PreviewImageRenderEndStatus status)
{
  IconPreview *ip = static_cast<IconPreview *>(customdata);

  if (ip->owner) {
    PreviewImage *prv_img = static_cast<PreviewImage *>(ip->owner);

    for (int i = 0; i < NUM_ICON_SIZES; i++) {
      if (ip->render_size[i]) {
        BKE_previewimg_render_end(prv_img, eIconSizes(i), status);
      }
    }

    ip->owner = nullptr;
  }
}

static void icon_preview_endjob(void *customdata)
{
  icon_preview_endjob(customdata, PRV_RENDER_STATUS_FINISHED);
}

/**
 * Background job to manage requests for deferred loading of previews from the hard drive.
 *
 * Launches a single job to manage all incoming preview requests. The job is kept running until all
 * preview requests are done loading (or it's otherwise aborted, e.g. by closing Blender).
 *
 * Note that this will use the OS thumbnail cache, i.e. load a preview from there or add it if not
 * there yet. These two cases may lead to different performance.
 *
 * This class also supports previews that need downloading before being available for loading from
 * disk. The download itself isn't managed by this class, but it should be informed about the
 * download status using #ED_preview_online_download_requested() and
 * #ED_preview_online_download_finished(). This only works for previews where
 * #BKE_previewimg_is_online() returns true.
 */
class PreviewLoadJob {
  enum class PreviewState : uint8_t {
    NotStarted,
    Downloading,
    LoadingFromDisk,
    /** Set when the request was fully handled and successfully got the preview. */
    Ready,
    /** Set to true if the request was handled but didn't result in a valid preview.
     * #PRV_TAG_DEFERRED_INVALID will be set in response. */
    Failed,
  };

  struct RequestedPreview {
    PreviewImage *preview;
    /** Requested size. */
    eIconSizes icon_size;
    std::atomic<PreviewState> state = PreviewState::NotStarted;

    /**
     * Indicates whether this RequestedPreview is queued in todo_queue_ (see below).
     *
     * Guard access with todo_queue_mutex_.
     *
     * This is _only_ used to ensure a request is never queued more than once. This field should
     * _not_ be used for any other purpose (as that would introduce race conditions).
     */
    bool is_queued = false;

    RequestedPreview(PreviewImage *preview, eIconSizes icon_size)
        : preview(preview), icon_size(icon_size)
    {
    }
    RequestedPreview(RequestedPreview &&other)
        : preview(other.preview), icon_size(other.icon_size), state(other.state.load())
    {
    }
  };

  /**
   * The previews that are still to be loaded from disk.
   * Don't access directly, use #todo_queue_push() and #todo_queue_pop().
   */
  ThreadQueue *todo_queue_; /* RequestedPreview * */
  /** Common mutex for accessing RequestedPreview::is_queued. */
  std::mutex todo_queue_mutex_;

  /** Push the RequestedPreview to the 'todo' queue, ensuring it is only queued once. */
  void todo_queue_push(RequestedPreview *request);
  /** Pop an item off the 'todo' queue, waiting at most wait_time_msec for an item to appear. */
  RequestedPreview *todo_queue_pop(int wait_time_msec);

  /**
   * Maps the file path identifying the preview + the requested icon size to the preview
   * request.
   *
   * Contains all unfinished preview requests. #update_fn() calls #finish_preview_request() on
   * loaded previews and removes them from this map.
   *
   * Only access with the mutex below!
   */
  Map<std::pair<std::string, eIconSizes>, std::unique_ptr<RequestedPreview>> requested_previews_;
  std::mutex requested_previews_mutex_;

 public:
  PreviewLoadJob();
  ~PreviewLoadJob();

  static PreviewLoadJob &ensure_job(wmWindowManager *wm, wmWindow *win);
  static void load_jobless(PreviewImage *preview, eIconSizes icon_size);
  static void on_download_requested(StringRef preview_full_filepath);
  static void on_download_completed(wmWindowManager *wm, StringRef preview_full_filepath);

  void push_load_request(PreviewImage *preview, eIconSizes icon_size);

 private:
  /**
   * The downloader might be done downloading previews and notify the preview system, even before
   * the preview loading job was started. Such previews are collected here. That way we can
   * recognize them as available on disk and the "is downloading" status can be skipped.
   *
   * Only call from the main thread!
   */
  static Set<std::string> &known_downloaded_previews();

  static void run_fn(void *customdata, wmJobWorkerStatus *worker_status);
  static void update_fn(void *customdata);
  static void end_fn(void *customdata);
  static void free_fn(void *customdata);

  /** Mark a single requested preview as being done, remove the request. */
  static void finish_request(RequestedPreview &request);
};

PreviewLoadJob::PreviewLoadJob() : todo_queue_(BLI_thread_queue_init()) {}

PreviewLoadJob::~PreviewLoadJob()
{
  BLI_thread_queue_free(todo_queue_);
}

Set<std::string> &PreviewLoadJob::known_downloaded_previews()
{
  BLI_assert(BLI_thread_is_main());
  static Set<std::string> known_downloaded_previews;
  return known_downloaded_previews;
}

PreviewLoadJob &PreviewLoadJob::ensure_job(wmWindowManager *wm, wmWindow *win)
{
  wmJob *wm_job = WM_jobs_get(
      wm, win, nullptr, "Loading previews...", eWM_JobFlag{}, WM_JOB_TYPE_LOAD_PREVIEW);

  if (!WM_jobs_is_running(wm_job)) {
    PreviewLoadJob *job_data = MEM_new<PreviewLoadJob>("PreviewLoadJobData");

    WM_jobs_customdata_set(wm_job, job_data, free_fn);
    WM_jobs_timer(wm_job, 0.1, NC_WINDOW, NC_WINDOW);
    WM_jobs_callbacks(wm_job, run_fn, nullptr, update_fn, end_fn);

    WM_jobs_start(wm, wm_job);
  }

  return *static_cast<PreviewLoadJob *>(WM_jobs_customdata_get(wm_job));
}

void PreviewLoadJob::load_jobless(PreviewImage *preview, const eIconSizes icon_size)
{
  PreviewLoadJob job_data{};

  job_data.push_load_request(preview, icon_size);

  wmJobWorkerStatus worker_status = {};
  run_fn(&job_data, &worker_status);
  update_fn(&job_data);
  end_fn(&job_data);
}

/**
 * Called when the preview is requested by the UI for drawing (or something close to that).
 */
void PreviewLoadJob::push_load_request(PreviewImage *preview, const eIconSizes icon_size)
{
  BLI_assert(BLI_thread_is_main());
  BLI_assert(preview->runtime->deferred_loading_data);
  BLI_assert_msg(!BKE_previewimg_is_rendering(preview, icon_size),
                 "Preview was already requested and is being loaded");
  std::optional<StringRefNull> path = BKE_previewimg_deferred_filepath_get(preview);
  if (!path) {
    BLI_assert_unreachable();
    return;
  }

  BKE_previewimg_render_start(preview, icon_size, true);

  const bool is_downloading = BKE_previewimg_is_online(preview) &&
                              !PreviewLoadJob::known_downloaded_previews().contains_as(*path);

  const std::pair key = std::make_pair(*path, icon_size);

  RequestedPreview *request = nullptr;
  {
    std::lock_guard lock(requested_previews_mutex_);

    /* Typically shouldn't happen, since previews are flagged with #PRV_RENDERING when loading,
     * which should prevent double requests. However, a #PreviewImage might be tagged for deletion
     * and recreated while a request is still pending. In that case, update the preview pointer.
     *
     * This happens when reloading online asset libraries with running preview downloads. The
     * assets are destructed then, the preview removed from the global cache (so it won't be
     * reused by the subsequent re-request) and tagged for freeing. */
    if (std::unique_ptr<RequestedPreview> *existing_request_uptr = requested_previews_.lookup_ptr(
            key))
    {
      RequestedPreview *existing_request = existing_request_uptr->get();
      if (existing_request->preview != preview) {
        /* This will free the preview if it's tagged with #PRV_TAG_DEFERRED_DELETE. That's
         * important since the global cache doesn't hold it anymore and therefore won't free it.
         * It's up to us here to end loading properly. */
        BKE_previewimg_render_end(existing_request->preview, icon_size, PRV_RENDER_STATUS_FAILED);
        existing_request->preview = preview;
      }
      return;
    }

    std::unique_ptr<RequestedPreview> new_request = std::make_unique<RequestedPreview>(preview,
                                                                                       icon_size);
    request = new_request.get();

    if (is_downloading) {
      request->state = PreviewState::Downloading;
    }
    else {
      request->state = PreviewState::LoadingFromDisk;
    }
    requested_previews_.add(key, std::move(new_request));
  }

  /* NOTE: The request gets pushed to the queue, even when state == PreviewState::Downloading, even
   * though PreviewLoadJob::run_fn immediately discards any queued request with that state. The
   * reason is that, between this push and that discard, the download may be done, and thus the
   * status of the request change. */
  this->todo_queue_push(request);
}

void PreviewLoadJob::todo_queue_push(PreviewLoadJob::RequestedPreview *request)
{
  std::lock_guard lock(this->todo_queue_mutex_);
  if (request->is_queued) {
    return;
  }

  BLI_thread_queue_push(todo_queue_, request, BLI_THREAD_QUEUE_WORK_PRIORITY_NORMAL);
  request->is_queued = true;
}

PreviewLoadJob::RequestedPreview *PreviewLoadJob::todo_queue_pop(const int wait_time_msec)
{
  RequestedPreview *request = static_cast<RequestedPreview *>(
      BLI_thread_queue_pop_timeout(this->todo_queue_, wait_time_msec));

  /* Only acquire the lock after the waiting. In the hypothetical case that
   * BLI_thread_queue_pop_timeout() would be called while this lock was acquired, nobody else would
   * be able to acquire the lock to actually push something onto the queue (because run_fn() below
   * is calling this pop function in quick succession; the only thing slowing it down is the
   * waiting time here).
   *
   * There still is a chance of a harmless race condition, when todo_queue_push() executes between
   * the pop above and the lock below. This is harmless, because:
   *
   * - The request is still marked as 'is queued', even though it was just popped off.
   * - This means the push function will not push it again.
   * - This can happen any number of times, making all the pushes no-ops.
   * - If this pop function gets called in the mean time, the queue does not contain this request.
   * - Once the lock below is acquired, the request is marked as 'not queued', and returned to the
   *   caller.
   *
   * The end state is that the request was still only returned by a pop function once, even though
   * its 'is queued' status was slightly misleading in the mean time. This is why that field is
   * documented as "do not use for anything else".
   */
  std::lock_guard lock(this->todo_queue_mutex_);

  if (!request) {
    return nullptr;
  }
  request->is_queued = false;
  return request;
}

/**
 * Static function, can be called at any time, even before the actual preview-loading job exists.
 */
void PreviewLoadJob::on_download_completed(wmWindowManager *wm,
                                           const StringRef preview_full_filepath)
{
  BLI_assert_msg(BLI_thread_is_main(),
                 "This function is meant to be called from external code, not from the job");

  PreviewLoadJob *load_job = static_cast<PreviewLoadJob *>(
      WM_jobs_customdata_from_type(wm, nullptr, WM_JOB_TYPE_LOAD_PREVIEW));
  if (!load_job) {
    PreviewLoadJob::known_downloaded_previews().add_as(preview_full_filepath);
    return;
  }

  bool has_request = false;
  /* Transition each preview request that uses this filepath from 'Downloading' to
   * 'LoadingFromDisk' and push it to the TODO queue, to trigger the actual loading from disk. */
  {
    std::lock_guard lock(load_job->requested_previews_mutex_);
    /* See if this download can be matched to one or more preview requests (from the UI). */
    for (int size = 0; size < NUM_ICON_SIZES; size++) {
      const std::pair key = std::make_pair(preview_full_filepath, eIconSizes(size));
      std::unique_ptr<RequestedPreview> *request_uptr = load_job->requested_previews_.lookup_ptr(
          key);
      if (!request_uptr) {
        continue;
      }
      has_request = true;
      RequestedPreview *request = request_uptr->get();

      if (request->state != PreviewState::Downloading) {
        continue;
      }

      /* Ensure the request from the UI gets answered, this is done in PreviewLoadJob::run_fn. */
      request->state = PreviewState::LoadingFromDisk;
      load_job->todo_queue_push(request);
    }
  }

  if (!has_request) {
    /* No pending request, but one might still be coming. Add it to the "known" downloaded
     * previews. */
    PreviewLoadJob::known_downloaded_previews().add_as(preview_full_filepath);
  }
}

void PreviewLoadJob::on_download_requested(const StringRef preview_full_filepath)
{
  /* Preview was requested. Allow the system to detect it as being downloaded by removing it from
   * the files known as "already downloaded". This way downloaded previews don't linger around
   * as "already downloaded" forever, and their downloading state can be recognized correctly. */
  PreviewLoadJob::known_downloaded_previews().remove_as(preview_full_filepath);
}

void PreviewLoadJob::run_fn(void *customdata, wmJobWorkerStatus *worker_status)
{
  PreviewLoadJob *job_data = static_cast<PreviewLoadJob *>(customdata);

  IMB_thumb_locks_acquire();

  bool has_work = true;
  /* Keep this loop running while there are any requests in the 'Downloading' or 'LoadingFromDisk'
   * state. This way previews that are done downloading don't need to be re-requested to actually
   * show up. */
  while (has_work && !worker_status->stop) {
    RequestedPreview *request = job_data->todo_queue_pop(100);

    if (worker_status->stop) {
      break;
    }

    has_work = request != nullptr;
    if (!has_work) {
      /* No immediate work; check if any previews are still pending. */
      std::lock_guard lock(job_data->requested_previews_mutex_);
      for (std::unique_ptr<RequestedPreview> &check_request :
           job_data->requested_previews_.values())
      {
        const PreviewState state = check_request->state.load();
        if (ELEM(state, PreviewState::Downloading, PreviewState::LoadingFromDisk)) {
          has_work = true;
          break;
        }
      }

      continue;
    }

    BLI_assert(request);
    /* Should never happen, because preview requests are only queued once they are 'started'. */
    BLI_assert(request->state != PreviewState::NotStarted);
    /* Should never happen, because the request only goes to these states at the end of this loop
     * body, in which case the request has already been popped off the queue. So it shouldn't be
     * seen here. */
    BLI_assert(!ELEM(request->state, PreviewState::Ready, PreviewState::Failed));

    if (request->state == PreviewState::Downloading) {
      /* This request can be safely remain popped off the queue, as the on_download_completed()
       * function will push it back on once the download is complete. This job loop will be kept
       * alive while things are downloading anyway, independent of todo_queue_ (see the `has_work`
       * check above). */
      continue;
    }

    PreviewImage *preview = request->preview;

    const std::optional<int> source = BKE_previewimg_deferred_thumb_source_get(preview);
    const std::optional<StringRefNull> filepath = BKE_previewimg_deferred_filepath_get(preview);

    if (!source || !filepath) {
      continue;
    }

    // printf("loading deferred %dx%d preview for %s\n", request->sizex, request->sizey, filepath);

    IMB_thumb_path_lock(filepath->c_str());
    ImBuf *thumb = IMB_thumb_manage(filepath->c_str(), THB_LARGE, ThumbSource(*source));
    IMB_thumb_path_unlock(filepath->c_str());

    if (thumb) {
      /* PreviewImage assumes premultiplied alpha. */
      IMB_premultiply_alpha(thumb);

      if (ED_preview_use_image_size(preview, request->icon_size)) {
        preview->w[request->icon_size] = thumb->x;
        preview->h[request->icon_size] = thumb->y;
        BLI_assert(preview->rect[request->icon_size] == nullptr);
        preview->rect[request->icon_size] = reinterpret_cast<uint *>(
            MEM_dupalloc(thumb->byte_data()));
      }
      else {
        icon_copy_rect(thumb,
                       preview->w[request->icon_size],
                       preview->h[request->icon_size],
                       preview->rect[request->icon_size]);
      }
      IMB_freeImBuf(thumb);
    }

    request->state = thumb ? PreviewState::Ready : PreviewState::Failed;
    worker_status->do_update = true;
  }

  IMB_thumb_locks_release();
}

/* Only execute on the main thread! */
void PreviewLoadJob::finish_request(RequestedPreview &request)
{
  BLI_assert(BLI_thread_is_main());

  PreviewImage *preview = request.preview;

  BKE_previewimg_render_end(preview,
                            request.icon_size,
                            request.state == PreviewState::Failed ? PRV_RENDER_STATUS_FAILED :
                                                                    PRV_RENDER_STATUS_FINISHED);
}

void PreviewLoadJob::update_fn(void *customdata)
{
  PreviewLoadJob *job_data = static_cast<PreviewLoadJob *>(customdata);

  Vector<std::pair<StringRef, eIconSizes>> finished_requests;

  std::lock_guard lock(job_data->requested_previews_mutex_);
  for (const auto item : job_data->requested_previews_.items()) {
    std::unique_ptr<RequestedPreview> &requested = item.value;

    /* Skip items that are not done loading yet. */
    if (ELEM(requested->state, PreviewState::Ready, PreviewState::Failed)) {
      finish_request(*requested);
      finished_requests.append(item.key);
    }
  }

  for (auto &key : finished_requests) {
    job_data->requested_previews_.remove(key);
  }
}

void PreviewLoadJob::end_fn(void *customdata)
{
  PreviewLoadJob *job_data = static_cast<PreviewLoadJob *>(customdata);

  std::lock_guard lock(job_data->requested_previews_mutex_);
  /* Finish any possibly remaining queued previews. */
  for (std::unique_ptr<RequestedPreview> &request : job_data->requested_previews_.values()) {
    finish_request(*request);
  }
  job_data->requested_previews_.clear();
}

void PreviewLoadJob::free_fn(void *customdata)
{
  MEM_delete(static_cast<PreviewLoadJob *>(customdata));
}

static void icon_preview_free(void *customdata)
{
  icon_preview_endjob(customdata, PRV_RENDER_STATUS_CANCELLED);

  IconPreview *ip = static_cast<IconPreview *>(customdata);

  if (ip->id_copy) {
    preview_id_copy_free(ip->id_copy);
  }

  MEM_delete(ip);
}

bool ED_preview_use_image_size(const PreviewImage *preview, eIconSizes size)
{
  return size == ICON_SIZE_PREVIEW && preview->runtime->deferred_loading_data;
}

bool ED_preview_id_render_is_supported(const ID *id, const char **r_disabled_hint)
{
  if (id == nullptr) {
    return false;
  }

  /* Get both the result and the "potential" disabled hint. After that we can decide if the
   * disabled hint needs to be returned to the caller. */
  const auto [result, disabled_hint] = [id]() -> std::pair<bool, const char *> {
    switch (GS(id->name)) {
      case ID_NT:
        return {false, RPT_("Node groups do not support automatic previews")};
      case ID_OB:
        return {object_preview_is_type_supported(id_cast<const Object *>(id)),
                RPT_("Object type does not support automatic previews")};
      case ID_GR:
        return {
            BKE_collection_contains_geometry_recursive(reinterpret_cast<const Collection *>(id)),
            RPT_("Collection does not contain object types that can be rendered for the automatic "
                 "preview")};
      case ID_SCE:
        return {scene_preview_is_supported(id_cast<const Scene *>(id)),
                RPT_("Scenes without a camera do not support previews")};
      case ID_BR:
        return {false, RPT_("Brushes do not support automatic previews")};
      case ID_MA:
        return {true, ""};
      case ID_TE:
        return {true, ""};
      case ID_WO:
        return {true, ""};
      case ID_LA:
        return {true, ""};
      case ID_IM:
        return {true, ""};
      case ID_AC:
        return {true, ""};
      case ID_SCR:
        return {false, RPT_("Screens do not support automatic previews")};
      default:
        BLI_assert(!BKE_previewimg_id_get_p(id));
        return {false, RPT_("Data-block type does not support automatic previews")};
    }
  }();

  if (result == false && disabled_hint && r_disabled_hint) {
    *r_disabled_hint = disabled_hint;
  }

  return result;
}

void ED_preview_icon_render(
    const bContext *C, Scene *scene, PreviewImage *prv_img, ID *id, eIconSizes icon_size)
{
  /* Deferred loading of previews from the file system. */
  if (prv_img->runtime->deferred_loading_data) {
    if (BKE_previewimg_is_rendering(prv_img, icon_size)) {
      /* Already in the queue, don't add it again. */
      return;
    }

    PreviewLoadJob::load_jobless(prv_img, icon_size);
    return;
  }

  /* Check if the ID supports the auto-generated previews at all. */
  if (!ED_preview_id_render_is_supported(id)) {
    return;
  }

  IconPreview ip = {nullptr};

  ED_preview_ensure_dbase(true);

  ip.bmain = CTX_data_main(C);
  if (GS(id->name) == ID_SCE) {
    Scene *icon_scene = reinterpret_cast<Scene *>(id);
    ip.scene = icon_scene;
    ip.depsgraph = BKE_scene_ensure_depsgraph(
        ip.bmain, ip.scene, BKE_view_layer_default_render(ip.scene));
    ip.active_object = nullptr;
  }
  else {
    ip.scene = scene;
    ip.depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    /* Control isn't given back to the caller until the preview is done. So we don't need to copy
     * the ID to avoid thread races. */
    ip.id_copy = duplicate_ids(id, true);
    ip.active_object = CTX_data_active_object(C);
  }
  ip.owner = BKE_previewimg_id_ensure(id);
  ip.id = id;

  BKE_previewimg_render_start(prv_img, icon_size, false);
  ip.render_size[icon_size] = true;

  wmJobWorkerStatus worker_status = {};
  icon_preview_startjob_all_sizes(&ip, &worker_status);

  icon_preview_endjob(&ip, PRV_RENDER_STATUS_FINISHED);

  if (ip.id_copy != nullptr) {
    preview_id_copy_free(ip.id_copy);
  }
}

void ED_preview_icon_job(
    const bContext *C, PreviewImage *prv_img, ID *id, eIconSizes icon_size, const bool delay)
{
  /* Deferred loading of previews from the file system. */
  if (prv_img->runtime->deferred_loading_data) {
    if (BKE_previewimg_is_rendering(prv_img, icon_size)) {
      /* Already in the queue, don't add it again. */
      return;
    }
    PreviewLoadJob &load_job = PreviewLoadJob::ensure_job(CTX_wm_manager(C), CTX_wm_window(C));
    load_job.push_load_request(prv_img, icon_size);

    return;
  }

  /* Check if the ID supports the auto-generated previews at all. */
  if (!ED_preview_id_render_is_supported(id)) {
    return;
  }

  IconPreview *ip, *old_ip;

  ED_preview_ensure_dbase(true);

  /* suspended start means it starts after 1 timer step, see WM_jobs_timer below */
  wmJob *wm_job = WM_jobs_get(CTX_wm_manager(C),
                              CTX_wm_window(C),
                              prv_img,
                              "Generating icon preview...",
                              WM_JOB_EXCL_RENDER,
                              WM_JOB_TYPE_RENDER_PREVIEW);

  ip = MEM_new_zeroed<IconPreview>("icon preview");

  /* render all resolutions from suspended job too */
  old_ip = static_cast<IconPreview *>(WM_jobs_customdata_get(wm_job));
  if (old_ip) {
    for (int i = 0; i < NUM_ICON_SIZES; i++) {
      ip->render_size[i] = old_ip->render_size[i];
      old_ip->render_size[i] = false;
    }
  }

  /* customdata for preview thread */
  ip->bmain = CTX_data_main(C);
  if (GS(id->name) == ID_SCE) {
    Scene *icon_scene = reinterpret_cast<Scene *>(id);
    ip->scene = icon_scene;
    ip->depsgraph = BKE_scene_ensure_depsgraph(
        ip->bmain, ip->scene, BKE_view_layer_default_render(ip->scene));
    ip->active_object = nullptr;
  }
  else {
    ip->depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    ip->scene = DEG_get_input_scene(ip->depsgraph);
    ip->id_copy = duplicate_ids(id, false);
    ip->active_object = CTX_data_active_object(C);
  }
  ip->owner = prv_img;
  ip->id = id;

  BKE_previewimg_render_start(prv_img, icon_size, true);
  ip->render_size[icon_size] = true;

  /* setup job */
  WM_jobs_customdata_set(wm_job, ip, icon_preview_free);
  WM_jobs_timer(wm_job, 0.1, NC_WINDOW, NC_WINDOW);
  /* Wait 2s to start rendering icon previews, to not bog down user interaction.
   * Particularly important for heavy scenes and Eevee using OpenGL that blocks
   * the user interface drawing. */
  WM_jobs_delay_start(wm_job, (delay) ? 2.0 : 0.0);
  WM_jobs_callbacks(
      wm_job, icon_preview_startjob_all_sizes, nullptr, nullptr, icon_preview_endjob);

  WM_jobs_start(CTX_wm_manager(C), wm_job);
}

void ED_preview_shader_job(const bContext *C,
                           const void *owner,
                           ID *id,
                           ID *parent,
                           MTex *slot,
                           int sizex,
                           int sizey,
                           ePreviewRenderMethod method)
{
  Object *ob = CTX_data_active_object(C);
  wmJob *wm_job;
  ShaderPreview *sp;
  Scene *scene = CTX_data_scene(C);
  const ID_Type id_type = GS(id->name);

  BLI_assert(BKE_previewimg_id_supports_jobs(id));

  /* Use workspace render only for buttons Window,
   * since the other previews are related to the datablock. */

  if (preview_method_is_render(method) && !ED_check_engine_supports_preview(scene)) {
    return;
  }

  ED_preview_ensure_dbase(true);

  wm_job = WM_jobs_get(CTX_wm_manager(C),
                       CTX_wm_window(C),
                       owner,
                       "Generating shader preview...",
                       WM_JOB_EXCL_RENDER,
                       WM_JOB_TYPE_RENDER_PREVIEW);
  sp = MEM_new_zeroed<ShaderPreview>("shader preview");

  /* customdata for preview thread */
  sp->scene = scene;
  sp->owner = owner;
  sp->sizex = sizex;
  sp->sizey = sizey;
  sp->pr_method = method;
  sp->id = id;
  sp->id_copy = duplicate_ids(id, false);
  sp->own_id_copy = true;
  sp->parent = parent;
  sp->slot = slot;
  sp->bmain = CTX_data_main(C);
  Material *ma = nullptr;

  /* hardcoded preview .blend for Eevee + Cycles, this should be solved
   * once with custom preview .blend path for external engines */

  /* grease pencil use its own preview file */
  if (id_type == ID_MA) {
    ma = id_cast<Material *>(id);
  }

  if ((ma == nullptr) || (ma->gp_style == nullptr)) {
    sp->pr_main = G.pr_main;
  }
  else {
    sp->pr_main = G_pr_main_grease_pencil;
  }

  if (ob && ob->totcol) {
    copy_v4_v4(sp->color, ob->color);
  }
  else {
    ARRAY_SET_ITEMS(sp->color, 0.0f, 0.0f, 0.0f, 1.0f);
  }

  /* setup job */
  WM_jobs_customdata_set(wm_job, sp, shader_preview_free);
  /* #NC_WINDOW rather than #NC_MATERIAL: the latter re-arms `sbuts->preview`, restarting the job
   * in an endless loop. */
  WM_jobs_timer(wm_job, 0.1, NC_WINDOW, NC_WINDOW);
  WM_jobs_callbacks(
      wm_job, common_preview_startjob, nullptr, shader_preview_updatejob, shader_preview_endjob);

  WM_jobs_start(CTX_wm_manager(C), wm_job);
}

void ED_preview_kill_jobs(wmWindowManager *wm, Main * /*bmain*/)
{
  if (wm) {
    /* This is called to stop all preview jobs before scene data changes, to
     * avoid invalid memory access. */
    WM_jobs_kill_type(wm, nullptr, WM_JOB_TYPE_RENDER_PREVIEW);
  }
}

void ED_preview_kill_jobs_for_id(wmWindowManager *wm, const ID *id)
{
  const PreviewImage *preview = BKE_previewimg_id_get(id);
  if (wm && preview) {
    WM_jobs_kill_type(wm, preview, WM_JOB_TYPE_RENDER_PREVIEW);
  }
}

void ED_preview_online_download_requested(const StringRef preview_full_filepath)
{
  PreviewLoadJob::on_download_requested(preview_full_filepath);
}

void ED_preview_online_download_finished(wmWindowManager *wm,
                                         const StringRef preview_full_filepath)
{
  PreviewLoadJob::on_download_completed(wm, preview_full_filepath);
}

void ED_preview_restart_work(const bContext *C)
{
  Main *bmain = CTX_data_main(C);

  if (!bmain->need_preview_render_restart) {
    return;
  }

  ID *id = nullptr;
  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    PreviewImage *preview = BKE_previewimg_id_get(id);
    if (!preview) {
      continue;
    }

    for (int i = 0; i < NUM_ICON_SIZES; i++) {
      if (BKE_previewimg_render_restart(preview, i)) {
        BKE_previewimg_clear_single(preview, eIconSizes(i));
        ui::icon_render_id(C, nullptr, id, eIconSizes(i), true);
      }
    }
  }
  FOREACH_MAIN_ID_END;

  bmain->need_preview_render_restart = false;
}

/** \} */

}  // namespace blender
