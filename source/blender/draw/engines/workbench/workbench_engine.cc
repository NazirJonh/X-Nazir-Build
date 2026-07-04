/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_rect.h"
#include "BLI_string.h"

#include "DNA_fluid_types.h"

#include "BKE_editmesh.hh"
#include "BKE_material.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_particle.h"
#include "BKE_report.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_windowmanager_types.h"
#include "ED_paint.hh"
#include "ED_view3d.hh"

#include "BLT_translation.hh"

#include "GPU_context.hh"
#include "IMB_imbuf_types.hh"

#include "RE_engine.h"
#include "RE_pipeline.h"

#include "draw_cache.hh"
#include "draw_common.hh"
#include "draw_sculpt.hh"
#include "draw_uv_space_view.hh"
#include "draw_view_data.hh"

#include "workbench_private.hh"

#include "workbench_engine.h" /* Own include. */

namespace blender {

namespace workbench {

using namespace draw;

/**
 * Which material paint channels a mesh carries per-vertex, indexed by #eMaterialPaintChannel.
 *
 * Only the scalar channels the prepass shader can read are ever set; Base Color goes through the
 * regular vertex color path and Custom has no shader slot.
 */
struct MaterialPropsUsage {
  bool used[PAINT_MATERIAL_CHANNEL_NUM] = {};

  bool any() const
  {
    for (const bool value : used) {
      if (value) {
        return true;
      }
    }
    return false;
  }
};

/**
 * Push constant declared by #workbench_prepass.bsl.hh for \a channel, or null when the shader has
 * no slot for it. Kept next to the draw code rather than in the channel descriptor table, since it
 * describes this shader rather than the paint data.
 */
static const char *material_props_uniform_name(const eMaterialPaintChannel channel)
{
  switch (channel) {
    case PAINT_MATERIAL_CHANNEL_METALLIC:
      return "use_vertex_metallic";
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
      return "use_vertex_roughness";
    case PAINT_MATERIAL_CHANNEL_SPECULAR:
      return "use_vertex_specular";
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
    case PAINT_MATERIAL_CHANNEL_CUSTOM:
      return nullptr;
  }
  return nullptr;
}

/**
 * Detects the material paint attributes present on \a ob.
 *
 * Presence alone is not enough: the shader reads these as float point attributes, and an unrelated
 * attribute that merely shares the name (a face-domain color, say) must not switch shading.
 */
static MaterialPropsUsage material_props_usage_get(const Object &ob)
{
  MaterialPropsUsage usage;
  if (ob.type != OB_MESH) {
    return usage;
  }

  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(ob);
  const bke::AttributeAccessor attributes = mesh.attributes();
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (material_props_uniform_name(info.channel) == nullptr) {
      continue;
    }
    const std::optional<bke::AttributeMetaData> meta_data = attributes.lookup_meta_data(
        info.attribute_name);
    usage.used[info.channel] = meta_data && meta_data->data_type == bke::AttrType::Float &&
                               meta_data->domain == bke::AttrDomain::Point;
  }
  return usage;
}

/* The UV-space display shows the unwrap of whatever the Image Editor shows: the active object
 * in paint or sculpt mode, and every object in multi-object edit mode. */
static bool uv_space_object_is_shown(const ObjectRef &ob_ref)
{
  const Object *ob = ob_ref.object;
  if (ob == nullptr || ob->type != OB_MESH) {
    return false;
  }
  if (ob->mode & OB_MODE_EDIT) {
    return true;
  }
  return ELEM(ob->mode, OB_MODE_TEXTURE_PAINT, OB_MODE_SCULPT, OB_MODE_VERTEX_PAINT);
}

class Instance : public DrawEngine {
 private:
  View view_ = {"DefaultView"};

  SceneState scene_state_;

  /* UV-space projection matrix, valid whenever `scene_state_.is_uv_space` is true. Seeded in
   * `init` and refreshed by #uv_to_clip_update once the sample jitter is known, so `draw_mesh`
   * never has to reach past `Instance` for it. */
  float4x4 uv_to_clip_ = float4x4::identity();

  SceneResources resources_;

  OpaquePass opaque_ps_;
  TransparentPass transparent_ps_;
  TransparentDepthPass transparent_depth_ps_;

  ShadowPass shadow_ps_;
  VolumePass volume_ps_;
  OutlinePass outline_ps_;
  DofPass dof_ps_;
  AntiAliasingPass anti_aliasing_ps_;

  /* An array of nullptr GPUMaterial pointers so we can call DRW_cache_object_surface_material_get.
   * They never get actually used. */
  Vector<GPUMaterial *> dummy_gpu_materials_ = {1, nullptr, {}};

  /* Used to detect any scene data update. */
  uint64_t depsgraph_last_update_ = 0;

  const char *hair_buffer_overflow_error_ = nullptr;

 public:
  const DRWContext *draw_ctx = nullptr;

  StringRefNull name_get() final
  {
    return "Workbench";
  }

  Span<const GPUMaterial *> get_dummy_gpu_materials(int material_count)
  {
    if (material_count > dummy_gpu_materials_.size()) {
      dummy_gpu_materials_.resize(material_count, nullptr);
    }
    return dummy_gpu_materials_.as_span().slice(IndexRange(material_count));
  };

  void init() final
  {
    this->draw_ctx = DRW_context_get();
    init(draw_ctx->depsgraph);
  }

  void init(Depsgraph *depsgraph, Object *camera_ob = nullptr)
  {
    this->draw_ctx = DRW_context_get();
    bool scene_updated = assign_if_different(depsgraph_last_update_,
                                             DEG_get_update_count(depsgraph));

    scene_state_.init(this->draw_ctx, scene_updated, camera_ob);

    if (scene_state_.is_uv_space) {
      /* Rebuilt with this sample's jitter in #draw, but a valid matrix is kept here as well so
       * the member is never stale between sync and draw. */
      this->uv_to_clip_update(float2(0.0f));
    }

    shadow_ps_.init(scene_state_, resources_);
    resources_.init(scene_state_, this->draw_ctx);

    outline_ps_.init(scene_state_);
    dof_ps_.init(scene_state_, this->draw_ctx);
    anti_aliasing_ps_.init(scene_state_);
  }

  /**
   * Recompute the UV-space projection for the current frame.
   *
   * `is_uv_space` is only ever true for the Image Editor's 2D render loop, which always supplies
   * a region (see `drw_draw_render_loop_2d`), so `region` is safe to dereference without a null
   * check.
   */
  void uv_to_clip_update(const float2 &jitter_ndc)
  {
    uv_to_clip_ = draw::uv_space_projection_get(
        this->draw_ctx->region->v2d.cur, int2(0, 0), jitter_ndc);
  }

  void begin_sync() final
  {
    resources_.material_buf.clear_and_trim();

    opaque_ps_.sync(scene_state_, resources_);
    transparent_ps_.sync(scene_state_, resources_);
    transparent_depth_ps_.sync(scene_state_, resources_);

    shadow_ps_.sync();
    volume_ps_.sync(resources_);
    outline_ps_.sync(resources_);
    dof_ps_.sync(resources_, this->draw_ctx);
    anti_aliasing_ps_.sync(scene_state_, resources_);

    hair_buffer_overflow_error_ = nullptr;
  }

  void end_sync() final
  {
    resources_.material_buf.push_update();
  }

  Material get_material(ObjectRef ob_ref, eV3DShadingColorType color_type, int slot = 0)
  {
    switch (color_type) {
      case V3D_SHADING_OBJECT_COLOR:
        return Material(*ob_ref.object);
      case V3D_SHADING_RANDOM_COLOR:
        return Material(*ob_ref.object, true);
      case V3D_SHADING_SINGLE_COLOR:
        return scene_state_.material_override;
      case V3D_SHADING_VERTEX_COLOR:
        return scene_state_.material_attribute_color;
      case V3D_SHADING_TEXTURE_COLOR:
        ATTR_FALLTHROUGH;
      case V3D_SHADING_MATERIAL_COLOR:
        if (blender::Material *_mat = BKE_object_material_get_eval(ob_ref.object, slot + 1)) {
          return Material(*_mat);
        }
        ATTR_FALLTHROUGH;
      default:
        return Material(*BKE_material_default_empty());
    }
  }

  void object_sync(ObjectRef &ob_ref, Manager &manager) final
  {
    if (scene_state_.render_finished) {
      return;
    }

    if (scene_state_.is_uv_space && !uv_space_object_is_shown(ob_ref)) {
      return;
    }

    Object *ob = ob_ref.object;
    if (!DRW_object_is_renderable(ob)) {
      return;
    }

    const ObjectState object_state = ObjectState(this->draw_ctx, scene_state_, resources_, ob);

    bool is_object_data_visible = (DRW_object_visibility_in_active_context(ob) &
                                   OB_VISIBLE_SELF) &&
                                  (ob->dt >= OB_SOLID || draw_ctx->is_scene_render());

    /* Volumes have no UV representation, so there is nothing to rasterize them into. Skipping the
     * whole block also keeps a gas domain from clearing #is_object_data_visible, which would stop
     * the mesh itself from drawing. */
    if (!scene_state_.is_uv_space && !(ob->base_flag & BASE_FROM_DUPLI)) {
      ModifierData *md = BKE_modifiers_findby_type(ob, eModifierType_Fluid);
      if (md && BKE_modifier_is_enabled(scene_state_.scene, md, eModifierMode_Realtime)) {
        FluidModifierData *fmd = reinterpret_cast<FluidModifierData *>(md);
        if (fmd->domain) {
          volume_ps_.object_sync_modifier(manager, resources_, scene_state_, ob_ref, md);

          if (fmd->domain->type == FLUID_DOMAIN_TYPE_GAS) {
            /* Do not draw solid in this case. */
            is_object_data_visible = false;
          }
        }
      }
    }

    ResourceHandleRange emitter_handle = {};

    if (is_object_data_visible) {
      if (object_state.sculpt_pbvh) {
        ResourceHandleRange handle = manager.unique_handle_for_sculpt(ob_ref);
        this->sculpt_sync(ob_ref, handle, object_state);
        emitter_handle = handle;
      }
      else if (ob->type == OB_MESH) {
        ResourceHandleRange handle = manager.unique_handle(ob_ref);
        this->mesh_sync(ob_ref, handle, object_state);
        emitter_handle = handle;
      }
      else if (ob->type == OB_POINTCLOUD) {
        this->pointcloud_sync(manager, ob_ref, object_state);
      }
      else if (ob->type == OB_CURVES) {
        this->curves_sync(manager, ob_ref, object_state);
      }
      else if (ob->type == OB_VOLUME) {
        if (scene_state_.shading.type != OB_WIRE) {
          volume_ps_.object_sync_volume(manager,
                                        resources_,
                                        scene_state_,
                                        ob_ref,
                                        get_material(ob_ref, object_state.color_type).base_color);
        }
      }
    }

    /* Hair strands carry no UV of their own, so #vert_curves has no UV-space path and would
     * rasterize them through the view matrices, scattering them across the unwrap. */
    if (!scene_state_.is_uv_space && ob->type == OB_MESH && ob->modifiers.first != nullptr) {
      for (ModifierData &md : ob->modifiers) {
        if (md.type != eModifierType_ParticleSystem) {
          continue;
        }
        ParticleSystem *psys = (reinterpret_cast<ParticleSystemModifierData *>(&md))->psys;
        if (!DRW_object_is_visible_psys_in_active_context(ob, psys)) {
          continue;
        }
        ParticleSettings *part = psys->part;
        const int draw_as = (part->draw_as == PART_DRAW_REND) ? part->ren_as : part->draw_as;

        if (draw_as == PART_DRAW_PATH) {
          this->hair_sync(manager, ob_ref, emitter_handle, object_state, psys, &md);
        }
      }
    }
  }

  template<typename F>
  void draw_to_mesh_pass(ObjectRef &ob_ref, bool is_transparent, F draw_callback)
  {
    const bool in_front = (ob_ref.object->dtx & OB_DRAW_IN_FRONT) != 0;

    if (scene_state_.xray_mode || is_transparent) {
      if (in_front) {
        draw_callback(transparent_ps_.accumulation_in_front_ps_);
        draw_callback(transparent_depth_ps_.in_front_ps_);
      }
      else {
        draw_callback(transparent_ps_.accumulation_ps_);
        draw_callback(transparent_depth_ps_.main_ps_);
      }
    }
    else {
      if (in_front) {
        draw_callback(opaque_ps_.gbuffer_in_front_ps_);
      }
      else {
        draw_callback(opaque_ps_.gbuffer_ps_);
      }
    }
  }

  void draw_mesh(ObjectRef &ob_ref,
                 Material &material,
                 gpu::Batch *batch,
                 ResourceHandleRange handle,
                 const MaterialTexture *texture = nullptr,
                 bool show_missing_texture = false,
                 const MaterialPropsUsage &material_props = {})
  {
    resources_.material_buf.append(material);
    int material_index = resources_.material_buf.size() - 1;

    if (show_missing_texture && (!texture || !texture->gpu.texture)) {
      texture = &resources_.missing_texture;
    }

    this->draw_to_mesh_pass(ob_ref, material.is_transparent(), [&](MeshPass &mesh_pass) {
      PassMain::Sub &pass = mesh_pass.get_subpass(eGeometryType::MESH, texture);
      for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
        if (const char *name = material_props_uniform_name(info.channel)) {
          pass.push_constant(name, material_props.used[info.channel]);
        }
      }
      if (scene_state_.is_uv_space) {
        /* Only pushed in UV mode: the 3D viewport would record 64 bytes of push constant per draw
         * that the shader never reads, since `is_uv_space` is already false from the sub-pass
         * default set in #MeshPass::get_subpass. Passed by pointer so the matrix is read at
         * submission time, after #Instance::draw has folded this frame's TAA jitter into it. */
        pass.push_constant("uv_to_clip", &uv_to_clip_);
      }
      pass.draw(batch, handle, material_index);
    });
  }

  void mesh_sync(ObjectRef &ob_ref, ResourceHandleRange handle, const ObjectState &object_state)
  {
    bool has_transparent_material = false;

    if (scene_state_.is_uv_space && ob_ref.object->type == OB_MESH) {
      /* The UV-space raster reads the UV attribute whatever the color type is, but none of the
       * surface batch getters below request UVs on their own: they only appear once the active UV
       * map is in the batch cache's `cd_used`. Request it here so the attribute is bound instead
       * of reading the zero default, which would collapse every triangle onto UV (0, 0). */
      DRW_cache_mesh_surface_uv_request(ob_ref.object);
    }

    /* Poly Paint: detect per-vertex material attributes so painted metallic/roughness/specular
     * display in Object Mode too, not only while the Sculpt Mode draw path is active. They only
     * override the solid base color, so they are ignored for texture and vertex-color shading. */
    MaterialPropsUsage material_props;
    if (object_state.color_type != V3D_SHADING_TEXTURE_COLOR &&
        object_state.color_type != V3D_SHADING_VERTEX_COLOR)
    {
      material_props = material_props_usage_get(*ob_ref.object);
    }
    const bool use_material_props = material_props.any();

    if (object_state.use_per_material_batches) {
      const int material_count = BKE_object_material_used_with_fallback_eval(*ob_ref.object);

      Span<gpu::Batch *> batches;
      if (object_state.color_type == V3D_SHADING_TEXTURE_COLOR) {
        batches = DRW_cache_mesh_surface_texpaint_get(ob_ref.object);
      }
      else if (use_material_props) {
        batches = DRW_cache_mesh_surface_shaded_material_props_get(
            ob_ref.object, this->get_dummy_gpu_materials(material_count));
      }
      else {
        batches = DRW_cache_object_surface_material_get(
            ob_ref.object, this->get_dummy_gpu_materials(material_count));
      }

      if (!batches.is_empty()) {
        for (auto i : IndexRange(material_count)) {
          if (batches[i] == nullptr) {
            continue;
          }

          int material_slot = i;
          Material mat = this->get_material(ob_ref, object_state.color_type, material_slot);
          has_transparent_material = has_transparent_material || mat.is_transparent();

          MaterialTexture texture;
          if (object_state.color_type == V3D_SHADING_TEXTURE_COLOR) {
            texture = MaterialTexture(ob_ref.object, material_slot);
          }

          this->draw_mesh(ob_ref,
                          mat,
                          batches[i],
                          handle,
                          &texture,
                          object_state.show_missing_texture,
                          material_props);
        }
      }
    }
    else {
      gpu::Batch *batch;
      if (object_state.color_type == V3D_SHADING_TEXTURE_COLOR) {
        batch = DRW_cache_mesh_surface_texpaint_single_get(ob_ref.object);
      }
      else if (object_state.color_type == V3D_SHADING_VERTEX_COLOR) {
        if (ob_ref.object->mode & OB_MODE_VERTEX_PAINT) {
          batch = DRW_cache_mesh_surface_vertpaint_get(ob_ref.object);
        }
        else {
          batch = DRW_cache_mesh_surface_sculptcolors_get(ob_ref.object);
        }
      }
      else if (use_material_props) {
        batch = DRW_cache_mesh_surface_material_props_get(ob_ref.object);
      }
      else {
        batch = DRW_cache_object_surface_get(ob_ref.object);
      }

      if (batch) {
        Material mat = this->get_material(ob_ref, object_state.color_type);
        has_transparent_material = has_transparent_material || mat.is_transparent();

        this->draw_mesh(ob_ref,
                        mat,
                        batch,
                        handle,
                        &object_state.image_paint_override,
                        false,
                        material_props);
      }
    }

    if (object_state.draw_shadow) {
      shadow_ps_.object_sync(scene_state_, ob_ref, handle, has_transparent_material);
    }
  }

  void sculpt_sync(ObjectRef &ob_ref, ResourceHandleRange handle, const ObjectState &object_state)
  {
    SculptBatchFeature features = SCULPT_BATCH_DEFAULT;
    if (object_state.color_type == V3D_SHADING_VERTEX_COLOR) {
      features |= SCULPT_BATCH_VERTEX_COLOR;
    }
    else if (object_state.color_type == V3D_SHADING_TEXTURE_COLOR) {
      features |= SCULPT_BATCH_UV;
    }

    if (scene_state_.is_uv_space) {
      /* The UV-space raster reads the UV attribute for every color type, not only for texture
       * color. Without it the attribute would be unbound and every triangle would collapse onto
       * UV (0, 0). */
      features |= SCULPT_BATCH_UV;
    }

    /* Only request the extra vertex buffers when the mesh actually carries painted channels;
     * every sculpt object would pay for them otherwise. */
    const MaterialPropsUsage material_props = material_props_usage_get(*ob_ref.object);
    if (material_props.any()) {
      features |= SCULPT_BATCH_MATERIAL_PROPS;
    }

    if (object_state.use_per_material_batches) {
      for (SculptBatch &batch : sculpt_batches_get(ob_ref.object, features)) {
        Material mat = this->get_material(ob_ref, object_state.color_type, batch.material_slot);
        if (scene_state_.show_paint_bvh_debug) {
          mat.base_color = batch.debug_color();
        }

        MaterialTexture texture;
        if (object_state.color_type == V3D_SHADING_TEXTURE_COLOR) {
          texture = MaterialTexture(ob_ref.object, batch.material_slot);
        }

        this->draw_mesh(ob_ref,
                        mat,
                        batch.batch,
                        handle,
                        &texture,
                        object_state.show_missing_texture,
                        material_props);
      }
    }
    else {
      Material mat = this->get_material(ob_ref, object_state.color_type);
      for (SculptBatch &batch : sculpt_batches_get(ob_ref.object, features)) {
        if (scene_state_.show_paint_bvh_debug) {
          mat.base_color = batch.debug_color();
        }

        this->draw_mesh(ob_ref,
                        mat,
                        batch.batch,
                        handle,
                        &object_state.image_paint_override,
                        false,
                        material_props);
      }
    }
  }

  void pointcloud_sync(Manager &manager, ObjectRef &ob_ref, const ObjectState &object_state)
  {
    ResourceHandleRange handle = manager.unique_handle(ob_ref);

    Material mat = this->get_material(ob_ref, object_state.color_type);
    resources_.material_buf.append(mat);
    int material_index = resources_.material_buf.size() - 1;

    this->draw_to_mesh_pass(ob_ref, mat.is_transparent(), [&](MeshPass &mesh_pass) {
      PassMain::Sub &pass =
          mesh_pass.get_subpass(eGeometryType::POINTCLOUD).sub("Point Cloud SubPass");
      gpu::Batch *batch = pointcloud_sub_pass_setup(pass, ob_ref.object);
      pass.draw(batch, handle, material_index);
    });
  }

  void hair_sync(Manager &manager,
                 ObjectRef &ob_ref,
                 ResourceHandle emitter_handle,
                 const ObjectState &object_state,
                 ParticleSystem *psys,
                 ModifierData *md)
  {
    ResourceHandle handle = manager.resource_handle_for_psys(ob_ref,
                                                             ob_ref.object->object_to_world());

    Material mat = this->get_material(ob_ref, object_state.color_type, psys->part->omat - 1);
    MaterialTexture texture;
    if (object_state.color_type == V3D_SHADING_TEXTURE_COLOR) {
      texture = MaterialTexture(ob_ref.object, psys->part->omat - 1);
    }
    resources_.material_buf.append(mat);
    int material_index = resources_.material_buf.size() - 1;

    this->draw_to_mesh_pass(ob_ref, mat.is_transparent(), [&](MeshPass &mesh_pass) {
      PassMain::Sub &pass =
          mesh_pass.get_subpass(eGeometryType::CURVES, &texture).sub("Hair SubPass");
      pass.push_constant("emitter_object_id", int(emitter_handle.raw()));
      gpu::Batch *batch = hair_sub_pass_setup(pass, scene_state_.scene, ob_ref, psys, md);
      pass.draw(batch, {ResourceID(handle)}, material_index);
    });
  }

  void curves_sync(Manager &manager, ObjectRef &ob_ref, const ObjectState &object_state)
  {
    ResourceHandleRange handle = manager.unique_handle(ob_ref);

    Material mat = this->get_material(ob_ref, object_state.color_type);
    resources_.material_buf.append(mat);
    int material_index = resources_.material_buf.size() - 1;

    this->draw_to_mesh_pass(ob_ref, mat.is_transparent(), [&](MeshPass &mesh_pass) {
      PassMain::Sub &pass = mesh_pass.get_subpass(eGeometryType::CURVES).sub("Curves SubPass");

      const char *error = nullptr;
      gpu::Batch *batch = curves_sub_pass_setup(pass, scene_state_.scene, ob_ref.object, error);
      if (error) {
        hair_buffer_overflow_error_ = error;
      }
      pass.draw(batch, handle, material_index);
    });
  }

  void draw(Manager &manager,
            gpu::Texture *depth_tx,
            gpu::Texture *depth_in_front_tx,
            gpu::Texture *color_tx)
  {
    int2 resolution = scene_state_.resolution;

    /** Always setup in-front depth, since Overlays can be updated without causing a Workbench
     * re-sync (See #113580). */
    bool needs_depth_in_front = !transparent_ps_.accumulation_in_front_ps_.is_empty() ||
                                (!opaque_ps_.gbuffer_in_front_ps_.is_empty() &&
                                 scene_state_.sample == 0);
    resources_.depth_in_front_tx.wrap(needs_depth_in_front ? depth_in_front_tx : nullptr);
    if (!needs_depth_in_front || opaque_ps_.gbuffer_in_front_ps_.is_empty()) {
      resources_.clear_in_front_fb.ensure(GPU_ATTACHMENT_TEXTURE(depth_in_front_tx));
      resources_.clear_in_front_fb.bind();
      GPU_framebuffer_clear_depth_stencil(resources_.clear_in_front_fb, 1.0f, 0x00);
    }

    resources_.depth_tx.wrap(depth_tx);
    resources_.color_tx.wrap(color_tx);

    if (scene_state_.render_finished) {
      /* Just copy back the already rendered result */
      anti_aliasing_ps_.draw(
          draw_ctx, manager, View::default_get(), scene_state_, resources_, depth_in_front_tx);
      return;
    }

    const float2 jitter_ndc = anti_aliasing_ps_.setup_view(view_, scene_state_);

    /* Frustum culling compares world-space object bounds against a frustum built from matrices
     * that no longer decide what lands on screen, since `out_position` comes from UV. Any object
     * away from the world origin would be culled, and zooming the Image Editor would shrink the
     * frustum further. */
    view_.visibility_test(!scene_state_.is_uv_space);

    if (scene_state_.is_uv_space) {
      /* The regular pipeline jitters the window matrix, which the UV raster does not use, so the
       * same offset has to be folded into the projection replacing it. Without this, sample
       * accumulation would keep adding identical samples and never anti-alias. */
      this->uv_to_clip_update(jitter_ndc);
    }

    GPUAttachment id_attachment = GPU_ATTACHMENT_NONE;
    if (scene_state_.draw_object_id) {
      resources_.object_id_tx.acquire_2d(resolution,
                                         gpu::TextureFormat::UINT_16,
                                         GPU_TEXTURE_USAGE_SHADER_READ |
                                             GPU_TEXTURE_USAGE_ATTACHMENT);
      id_attachment = GPU_ATTACHMENT_TEXTURE(resources_.object_id_tx);
    }
    resources_.clear_fb.ensure(GPU_ATTACHMENT_TEXTURE(resources_.depth_tx),
                               GPU_ATTACHMENT_TEXTURE(resources_.color_tx),
                               id_attachment);
    resources_.clear_fb.bind();
    std::array<double4, 2> clear_colors = {double4(scene_state_.background_color), double4(0.0f)};
    GPU_framebuffer_multi_clear(resources_.clear_fb, clear_colors);
    GPU_framebuffer_clear_depth_stencil(resources_.clear_fb, 1.0f, 0x00);

    opaque_ps_.draw(
        manager, view_, resources_, resolution, scene_state_.draw_shadows ? &shadow_ps_ : nullptr);
    transparent_ps_.draw(manager, view_, resources_, resolution);
    transparent_depth_ps_.draw(manager, view_, resources_);

    volume_ps_.draw(manager, view_, resources_);
    outline_ps_.draw(manager, resources_);
    dof_ps_.draw(manager, view_, resources_, resolution);
    anti_aliasing_ps_.draw(draw_ctx, manager, view_, scene_state_, resources_, depth_in_front_tx);

    resources_.object_id_tx.release();
  }

  void draw_viewport(Manager &manager,
                     gpu::Texture *depth_tx,
                     gpu::Texture *depth_in_front_tx,
                     gpu::Texture *color_tx)
  {
    this->draw(manager, depth_tx, depth_in_front_tx, color_tx);

    if (scene_state_.sample + 1 < scene_state_.samples_len) {
      DRW_viewport_request_redraw();
    }

    if (hair_buffer_overflow_error_) {
      STRNCPY(info, hair_buffer_overflow_error_);
    }
    else {
      STRNCPY(info, "");
    }
  }

  void draw(Manager &manager) final
  {
    DefaultTextureList *dtxl = draw_ctx->viewport_texture_list_get();

    DRW_submission_start();
    if (draw_ctx->is_viewport_image_render()) {
      draw_image_render(manager, dtxl->depth, dtxl->depth_in_front, dtxl->color);
    }
    else {
      draw_viewport(manager, dtxl->depth, dtxl->depth_in_front, dtxl->color);
    }
    DRW_submission_end();
  }

  void draw_image_render(Manager &manager,
                         gpu::Texture *depth_tx,
                         gpu::Texture *depth_in_front_tx,
                         gpu::Texture *color_tx,
                         RenderEngine *engine = nullptr)
  {
    if (scene_state_.render_finished) {
      /* This can happen in viewport animation renders, if the scene didn't have any updates
       * between frames. */
      this->draw(manager, depth_tx, depth_in_front_tx, color_tx);
      return;
    }

    BLI_assert(scene_state_.sample == 0);
    for (auto i : IndexRange(scene_state_.samples_len)) {
      if (hair_buffer_overflow_error_) {
        RE_engine_set_error_message(engine, hair_buffer_overflow_error_);
      }

      if (engine && RE_engine_test_break(engine)) {
        break;
      }
      if (i != 0) {
        scene_state_.sample = i;
        /* Re-sync anything dependent on scene_state.sample. */
        resources_.init(scene_state_, draw_ctx);
        dof_ps_.init(scene_state_, draw_ctx);
        anti_aliasing_ps_.sync(scene_state_, resources_);
      }
      this->draw(manager, depth_tx, depth_in_front_tx, color_tx);
      /* Metal: Perform render step between samples to allow flushing of freed GPUBackend
       * resources. Vulkan: Perform render step between samples to avoid allocation of a high
       * amount of command buffer memory that can eventually result in out-of-memory errors or a
       * TDR when submitted as one large command buffer. */
      if (ELEM(GPU_backend_get_type(), GPU_BACKEND_METAL, GPU_BACKEND_VULKAN)) {
        GPU_flush();
      }
      GPU_render_step();
    }
  }
};

DrawEngine *Engine::create_instance()
{
  return new Instance();
}

void Engine::free_static()
{
  ShaderCache::release();
}

}  // namespace workbench

/* -------------------------------------------------------------------- */
/** \name Interface with legacy C DRW manager
 * \{ */

/* RENDER */

static bool workbench_render_framebuffers_init(const DRWContext *draw_ctx)
{
  /* For image render, allocate own buffers because we don't have a viewport. */
  const float2 viewport_size = draw_ctx->viewport_size_get();
  const int2 size = {int(viewport_size.x), int(viewport_size.y)};

  DefaultTextureList *dtxl = draw_ctx->viewport_texture_list_get();

  /* When doing a multi view rendering the first view will allocate the buffers
   * the other views will reuse these buffers */
  if (dtxl->color == nullptr) {
    BLI_assert(dtxl->depth == nullptr);
    eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL;
    dtxl->color = GPU_texture_create_2d(
        "txl.color", size.x, size.y, 1, gpu::TextureFormat::SFLOAT_16_16_16_16, usage, nullptr);
    dtxl->depth = GPU_texture_create_2d("txl.depth",
                                        size.x,
                                        size.y,
                                        1,
                                        gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8,
                                        usage,
                                        nullptr);
    dtxl->depth_in_front = GPU_texture_create_2d("txl.depth_in_front",
                                                 size.x,
                                                 size.y,
                                                 1,
                                                 gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8,
                                                 usage,
                                                 nullptr);
  }

  if (!(dtxl->depth && dtxl->color && dtxl->depth_in_front)) {
    return false;
  }

  DefaultFramebufferList *dfbl = draw_ctx->viewport_framebuffer_list_get();

  GPU_framebuffer_ensure_config(
      &dfbl->default_fb,
      {GPU_ATTACHMENT_TEXTURE(dtxl->depth), GPU_ATTACHMENT_TEXTURE(dtxl->color)});

  GPU_framebuffer_ensure_config(&dfbl->depth_only_fb,
                                {GPU_ATTACHMENT_TEXTURE(dtxl->depth), GPU_ATTACHMENT_NONE});

  GPU_framebuffer_ensure_config(&dfbl->color_only_fb,
                                {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(dtxl->color)});

  return GPU_framebuffer_check_valid(dfbl->default_fb, nullptr) &&
         GPU_framebuffer_check_valid(dfbl->color_only_fb, nullptr) &&
         GPU_framebuffer_check_valid(dfbl->depth_only_fb, nullptr);
}

static void write_render_color_output(RenderLayer *layer,
                                      const char *viewname,
                                      gpu::FrameBuffer *fb,
                                      const rcti *rect)
{
  RenderPass *rp = RE_pass_find_by_name(layer, RE_PASSNAME_COMBINED, viewname);
  if (rp) {
    GPU_framebuffer_bind(fb);
    GPU_framebuffer_read_color(fb,
                               rect->xmin,
                               rect->ymin,
                               BLI_rcti_size_x(rect),
                               BLI_rcti_size_y(rect),
                               4,
                               0,
                               GPU_DATA_FLOAT,
                               rp->ibuf->float_data_for_write());
  }
}

static void write_render_z_output(RenderLayer *layer,
                                  const char *viewname,
                                  gpu::FrameBuffer *fb,
                                  const rcti *rect,
                                  const float4x4 &winmat)
{
  RenderPass *rp = RE_pass_find_by_name(layer, RE_PASSNAME_DEPTH, viewname);
  if (rp) {
    GPU_framebuffer_bind(fb);
    GPU_framebuffer_read_depth(fb,
                               rect->xmin,
                               rect->ymin,
                               BLI_rcti_size_x(rect),
                               BLI_rcti_size_y(rect),
                               GPU_DATA_FLOAT,
                               rp->ibuf->float_data_for_write());

    int pix_num = BLI_rcti_size_x(rect) * BLI_rcti_size_y(rect);

    /* Convert GPU depth [0..1] to view Z [near..far] */
    if (draw::View::default_get().is_persp()) {
      for (float &z : MutableSpan(rp->ibuf->float_data_for_write(), pix_num)) {
        if (z == 1.0f) {
          z = 1e10f; /* Background */
        }
        else {
          z = z * 2.0f - 1.0f;
          z = winmat[3][2] / (z + winmat[2][2]);
        }
      }
    }
    else {
      /* Keep in mind, near and far distance are negatives. */
      float near = draw::View::default_get().near_clip();
      float far = draw::View::default_get().far_clip();
      float range = fabsf(far - near);

      for (float &z : MutableSpan(rp->ibuf->float_data_for_write(), pix_num)) {
        if (z == 1.0f) {
          z = 1e10f; /* Background */
        }
        else {
          z = z * range - near;
        }
      }
    }
  }
}

static void workbench_render_to_image(RenderEngine *engine, RenderLayer *layer, const rcti rect)
{
  using namespace blender::draw;
  const DRWContext *draw_ctx = DRW_context_get();

  if (!workbench_render_framebuffers_init(draw_ctx)) {
    RE_engine_report(engine, RPT_ERROR, "Failed to allocate GPU buffers");
    return;
  }

  /* Setup */
  DefaultFramebufferList *dfbl = draw_ctx->viewport_framebuffer_list_get();
  Depsgraph *depsgraph = draw_ctx->depsgraph;

  workbench::Instance instance;

  /* TODO(sergey): Shall render hold pointer to an evaluated camera instead? */
  Object *camera_ob = DEG_get_evaluated(depsgraph, RE_GetCamera(engine->re));

  /* Set the perspective, view and window matrix. */
  float4x4 winmat, viewmat, viewinv;
  RE_GetCameraWindow(engine->re, camera_ob, winmat.ptr());
  RE_GetCameraModelMatrix(engine->re, camera_ob, viewinv.ptr());
  viewmat = math::invert(viewinv);

  /* Render */
  /* TODO: Remove old draw manager calls. */
  DRW_cache_restart();
  draw::View::default_set(float4x4(viewmat), float4x4(winmat));

  instance.init(depsgraph, camera_ob);

  draw::Manager &manager = *DRW_manager_get();
  manager.begin_sync();

  instance.begin_sync();
  DRW_render_object_iter(
      engine,
      depsgraph,
      [&](draw::ObjectRef &ob_ref, RenderEngine * /*engine*/, Depsgraph * /*depsgraph*/) {
        instance.object_sync(ob_ref, manager);
      });
  instance.end_sync();

  manager.end_sync();

  DRW_submission_start();

  DefaultTextureList &dtxl = *draw_ctx->viewport_texture_list_get();
  instance.draw_image_render(manager, dtxl.depth, dtxl.depth_in_front, dtxl.color, engine);

  DRW_submission_end();

  /* Write image */
  const char *viewname = RE_GetActiveRenderView(engine->re);
  write_render_color_output(layer, viewname, dfbl->default_fb, &rect);
  write_render_z_output(layer, viewname, dfbl->default_fb, &rect, winmat);
}

static void workbench_render_update_passes(RenderEngine *engine,
                                           Scene *scene,
                                           ViewLayer *view_layer)
{
  if (view_layer->passflag & SCE_PASS_COMBINED) {
    RE_engine_register_pass(engine, scene, view_layer, RE_PASSNAME_COMBINED, 4, "RGBA", SOCK_RGBA);
  }
  if (view_layer->passflag & SCE_PASS_DEPTH) {
    RE_engine_register_pass(engine, scene, view_layer, RE_PASSNAME_DEPTH, 1, "Z", SOCK_FLOAT);
  }
}

static void workbench_render(RenderEngine *engine, Depsgraph *depsgraph)
{
  DRW_render_to_image(engine, depsgraph, workbench_render_to_image, [](RenderResult *) {});
}

RenderEngineType DRW_engine_viewport_workbench_type = {
    /*next*/ nullptr,
    /*prev*/ nullptr,
    /*idname*/ "BLENDER_WORKBENCH",
    /*name*/ N_("Workbench"),
    /*flag*/ RE_INTERNAL | RE_USE_STEREO_VIEWPORT | RE_USE_GPU_CONTEXT,
    /*update*/ nullptr,
    /*render*/ &workbench_render,
    /*render_frame_finish*/ nullptr,
    /*draw*/ nullptr,
    /*bake*/ nullptr,
    /*view_update*/ nullptr,
    /*view_draw*/ nullptr,
    /*update_script_node*/ nullptr,
    /*update_render_passes*/ &workbench_render_update_passes,
    /*update_custom_camera*/ nullptr,
    /*draw_engine*/ nullptr,
    /*rna_ext*/
    {
        /*data*/ nullptr,
        /*srna*/ nullptr,
        /*call*/ nullptr,
    },
};

/** \} */

}  // namespace blender
