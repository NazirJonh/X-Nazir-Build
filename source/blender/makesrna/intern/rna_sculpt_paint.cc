/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <cstdlib>

#include <fmt/format.h>

#include "BLI_math_base.h"

#include "BLT_translation.hh"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "rna_internal.hh"

#include "DNA_brush_types.h"
#include "DNA_object_types.h"
#include "DNA_image_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.h"
#include "BKE_colorband.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_sync.hh"

#include "IMB_imbuf.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "bmesh.hh"

namespace blender {

/* Mirrors the descriptor table in #BKE_paint_material_channels. */
const EnumPropertyItem rna_enum_material_paint_channel_items[] = {
    {PAINT_MATERIAL_CHANNEL_BASE_COLOR, "BASE_COLOR", 0, "Base Color", "Base color channel"},
    {PAINT_MATERIAL_CHANNEL_METALLIC, "METALLIC", 0, "Metallic", "Metallic channel"},
    {PAINT_MATERIAL_CHANNEL_ROUGHNESS, "ROUGHNESS", 0, "Roughness", "Roughness channel"},
    {PAINT_MATERIAL_CHANNEL_SPECULAR, "SPECULAR", 0, "Specular", "Specular channel"},
    {PAINT_MATERIAL_CHANNEL_NORMAL, "NORMAL", 0, "Normal", "Tangent-space normal map channel"},
    {PAINT_MATERIAL_CHANNEL_CUSTOM,
     "CUSTOM",
     0,
     "Custom",
     "User-named float attribute, vertex painting only"},
    {PAINT_MATERIAL_CHANNEL_HEIGHT, "HEIGHT", 0, "Height", "Scalar height/displacement channel"},
    {PAINT_MATERIAL_CHANNEL_ALPHA,
     "ALPHA",
     0,
     "Alpha",
     "Scalar alpha channel; also masks writes to other active channels while enabled"},
    {PAINT_MATERIAL_CHANNEL_AO, "AO", 0, "AO", "Ambient occlusion channel"},
    {PAINT_MATERIAL_CHANNEL_EMISSION, "EMISSION", 0, "Emission", "Emission color channel"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Bit-flag values for #Paint.visible_material_channels. Must not reuse
 * #rna_enum_material_paint_channel_items: that table stores channel indices (0..9), while
 * PROP_ENUM_FLAG requires each item to be a unique power-of-two bit. */
static const EnumPropertyItem rna_enum_visible_material_paint_channel_items[] = {
    {1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR, "BASE_COLOR", 0, "Base Color", ""},
    {1 << PAINT_MATERIAL_CHANNEL_METALLIC, "METALLIC", 0, "Metallic", ""},
    {1 << PAINT_MATERIAL_CHANNEL_ROUGHNESS, "ROUGHNESS", 0, "Roughness", ""},
    {1 << PAINT_MATERIAL_CHANNEL_SPECULAR, "SPECULAR", 0, "Specular", ""},
    {1 << PAINT_MATERIAL_CHANNEL_NORMAL, "NORMAL", 0, "Normal", ""},
    {1 << PAINT_MATERIAL_CHANNEL_HEIGHT, "HEIGHT", 0, "Height", ""},
    {1 << PAINT_MATERIAL_CHANNEL_ALPHA, "ALPHA", 0, "Alpha", ""},
    {1 << PAINT_MATERIAL_CHANNEL_AO, "AO", 0, "AO", ""},
    {1 << PAINT_MATERIAL_CHANNEL_EMISSION, "EMISSION", 0, "Emission", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

const EnumPropertyItem rna_enum_particle_edit_hair_brush_items[] = {
    {PE_BRUSH_COMB, "COMB", 0, "Comb", "Comb hairs"},
    {PE_BRUSH_SMOOTH, "SMOOTH", 0, "Smooth", "Smooth hairs"},
    {PE_BRUSH_ADD, "ADD", 0, "Add", "Add hairs"},
    {PE_BRUSH_LENGTH, "LENGTH", 0, "Length", "Make hairs longer or shorter"},
    {PE_BRUSH_PUFF, "PUFF", 0, "Puff", "Make hairs stand up"},
    {PE_BRUSH_CUT, "CUT", 0, "Cut", "Cut hairs"},
    {PE_BRUSH_WEIGHT, "WEIGHT", 0, "Weight", "Weight hair particles"},
    {0, nullptr, 0, nullptr, nullptr},
};

#ifndef RNA_RUNTIME
static const EnumPropertyItem rna_enum_gpencil_lock_axis_items[] = {
    {GP_LOCKAXIS_VIEW,
     "VIEW",
     ICON_RESTRICT_VIEW_ON,
     "View",
     "Align strokes to current view plane"},
    {GP_LOCKAXIS_Y,
     "AXIS_Y",
     ICON_AXIS_FRONT,
     "Front (X-Z)",
     "Project strokes to plane locked to Y"},
    {GP_LOCKAXIS_X,
     "AXIS_X",
     ICON_AXIS_SIDE,
     "Side (Y-Z)",
     "Project strokes to plane locked to X"},
    {GP_LOCKAXIS_Z, "AXIS_Z", ICON_AXIS_TOP, "Top (X-Y)", "Project strokes to plane locked to Z"},
    {GP_LOCKAXIS_CURSOR,
     "CURSOR",
     ICON_PIVOT_CURSOR,
     "Cursor",
     "Align strokes to current 3D cursor orientation"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_gpencil_paint_mode[] = {
    {GPPAINT_FLAG_USE_MATERIAL,
     "MATERIAL",
     0,
     "Material",
     "Paint using the active material base color"},
    {GPPAINT_FLAG_USE_VERTEXCOLOR,
     "VERTEXCOLOR",
     0,
     "Color Attribute",
     "Paint the material with a color attribute"},
    {0, nullptr, 0, nullptr, nullptr},
};
#endif

static const EnumPropertyItem rna_enum_canvas_source_items[] = {
    {PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE, "COLOR_ATTRIBUTE", 0, "Color Attribute", ""},
    {PAINT_CANVAS_SOURCE_MATERIAL, "MATERIAL", 0, "Material", ""},
    {PAINT_CANVAS_SOURCE_IMAGE, "IMAGE", 0, "Image", ""},
    {PAINT_CANVAS_SOURCE_MATERIAL_PAINT,
     "MATERIAL_PAINT",
     0,
     "PolyPaint",
     "Paint per-vertex material attribute channels"},
    {0, nullptr, 0, nullptr, nullptr},
};

const EnumPropertyItem rna_enum_symmetrize_direction_items[] = {
    {BMO_SYMMETRIZE_NEGATIVE_X, "NEGATIVE_X", 0, "-X to +X", ""},
    {BMO_SYMMETRIZE_POSITIVE_X, "POSITIVE_X", 0, "+X to -X", ""},

    {BMO_SYMMETRIZE_NEGATIVE_Y, "NEGATIVE_Y", 0, "-Y to +Y", ""},
    {BMO_SYMMETRIZE_POSITIVE_Y, "POSITIVE_Y", 0, "+Y to -Y", ""},

    {BMO_SYMMETRIZE_NEGATIVE_Z, "NEGATIVE_Z", 0, "-Z to +Z", ""},
    {BMO_SYMMETRIZE_POSITIVE_Z, "POSITIVE_Z", 0, "+Z to -Z", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

}  // namespace blender

#ifdef RNA_RUNTIME
#  include "MEM_guardedalloc.h"

#  include "DNA_mesh_types.h"
#  include "DNA_pointcloud_types.h"

#  include "BKE_brush.hh"
#  include "BKE_collection.hh"
#  include "BKE_colortools.hh"
#  include "BKE_context.hh"
#  include "BKE_curve_patch.hh"
#  include "BKE_curves.hh"
#  include "BKE_gpencil_legacy.h"
#  include "BKE_image.hh"
#  include "BKE_layer.hh"
#  include "BKE_lib_id.hh"
#  include "BKE_material.hh"
#  include "BKE_mesh.h"
#  include "BKE_mesh.hh"
#  include "BKE_object.hh"
#  include "BKE_paint.hh"
#  include "BKE_paint_types.hh"
#  include "BKE_particle.h"
#  include "BKE_pointcache.h"
#  include "BKE_pointcloud.hh"

#  include "DEG_depsgraph.hh"
#  include "DEG_depsgraph_query.hh"

#  include "ED_gpencil_legacy.hh"
#  include "ED_image.hh"
#  include "ED_object.hh"
#  include "ED_paint.hh"
#  include "ED_particle.hh"

namespace blender {

const EnumPropertyItem rna_enum_particle_edit_disconnected_hair_brush_items[] = {
    {PE_BRUSH_COMB, "COMB", 0, "Comb", "Comb hairs"},
    {PE_BRUSH_SMOOTH, "SMOOTH", 0, "Smooth", "Smooth hairs"},
    {PE_BRUSH_LENGTH, "LENGTH", 0, "Length", "Make hairs longer or shorter"},
    {PE_BRUSH_CUT, "CUT", 0, "Cut", "Cut hairs"},
    {PE_BRUSH_WEIGHT, "WEIGHT", 0, "Weight", "Weight hair particles"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem particle_edit_cache_brush_items[] = {
    {PE_BRUSH_COMB, "COMB", 0, "Comb", "Comb paths"},
    {PE_BRUSH_SMOOTH, "SMOOTH", 0, "Smooth", "Smooth paths"},
    {PE_BRUSH_LENGTH, "LENGTH", 0, "Length", "Make paths longer or shorter"},
    {0, nullptr, 0, nullptr, nullptr},
};

static PointerRNA rna_ParticleEdit_brush_get(PointerRNA *ptr)
{
  ParticleEditSettings *pset = static_cast<ParticleEditSettings *>(ptr->data);
  ParticleBrushData *brush = nullptr;

  brush = &pset->brush[pset->brushtype];

  return RNA_pointer_create_with_parent(*ptr, RNA_ParticleBrush, brush);
}

static PointerRNA rna_ParticleBrush_curve_get(PointerRNA * /*ptr*/)
{
  return PointerRNA_NULL;
}

static void rna_ParticleEdit_redo(bContext *C, PointerRNA * /*ptr*/)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);
  PTCacheEdit *edit = PE_get_current(depsgraph, scene, ob);

  if (!edit) {
    return;
  }

  if (ob) {
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  }

  if (edit->psys) {
    BKE_particle_batch_cache_dirty_tag(edit->psys, BKE_PARTICLE_BATCH_DIRTY_ALL);
    psys_free_path_cache(edit->psys, edit);
  }
  DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);
}

static void rna_ParticleEdit_update(bContext *C, PointerRNA * /*ptr*/)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);

  if (ob) {
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  }

  /* Sync tool setting changes from original to evaluated scenes. */
  DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);
}

static void rna_ParticleEdit_tool_set(PointerRNA *ptr, int value)
{
  ParticleEditSettings *pset = static_cast<ParticleEditSettings *>(ptr->data);

  /* redraw hair completely if weight brush is/was used */
  if ((pset->brushtype == PE_BRUSH_WEIGHT || value == PE_BRUSH_WEIGHT) && pset->object) {
    Object *ob = pset->object;
    if (ob) {
      DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
      WM_main_add_notifier(NC_OBJECT | ND_PARTICLE | NA_EDITED, nullptr);
    }
  }

  pset->brushtype = value;
}
static const EnumPropertyItem *rna_ParticleEdit_tool_itemf(bContext *C,
                                                           PointerRNA * /*ptr*/,
                                                           PropertyRNA * /*prop*/,
                                                           bool * /*r_free*/)
{
  if (C) {
    const Main *bmain = CTX_data_main(C);
    const Scene *scene = CTX_data_scene(C);
    ViewLayer *view_layer = CTX_data_view_layer(C);
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    Object *ob = BKE_view_layer_active_object_get(view_layer);
#  if 0
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Scene *scene = CTX_data_scene(C);
  PTCacheEdit *edit = PE_get_current(depsgraph, scene, ob);
  ParticleSystem *psys = edit ? edit->psys : nullptr;
#  else
    /* use this rather than PE_get_current() - because the editing cache is
     * dependent on the cache being updated which can happen after this UI
     * draws causing a glitch #28883. */
    ParticleSystem *psys = psys_get_current(ob);
#  endif

    if (psys) {
      if (psys->flag & PSYS_GLOBAL_HAIR) {
        return rna_enum_particle_edit_disconnected_hair_brush_items;
      }
      else {
        return rna_enum_particle_edit_hair_brush_items;
      }
    }
  }

  return particle_edit_cache_brush_items;
}

static bool rna_ParticleEdit_editable_get(PointerRNA *ptr)
{
  ParticleEditSettings *pset = static_cast<ParticleEditSettings *>(ptr->data);

  return (pset->object && pset->scene && PE_get_current(nullptr, pset->scene, pset->object));
}
static bool rna_ParticleEdit_hair_get(PointerRNA *ptr)
{
  ParticleEditSettings *pset = static_cast<ParticleEditSettings *>(ptr->data);

  if (pset->scene) {
    PTCacheEdit *edit = PE_get_current(nullptr, pset->scene, pset->object);

    return (edit && edit->psys);
  }

  return 0;
}

static std::optional<std::string> rna_ParticleEdit_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.particle_edit";
}

static PointerRNA rna_Paint_brush_get(PointerRNA *ptr)
{
  Paint *paint = static_cast<Paint *>(ptr->data);
  Brush *brush = BKE_paint_brush(paint);
  if (!brush) {
    return PointerRNA_NULL;
  }
  return RNA_id_pointer_create(&brush->id);
}

static bool rna_Paint_brush_poll(PointerRNA *ptr, PointerRNA value)
{
  const Paint *paint = static_cast<Paint *>(ptr->data);
  const Brush *brush = static_cast<Brush *>(value.data);

  return (brush == nullptr) || (paint->runtime->ob_mode & brush->ob_mode) != 0;
}

static void rna_Sculpt_update(bContext *C, PointerRNA * /*ptr*/)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);

  if (ob) {
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_OBJECT | ND_MODIFIER, ob);
  }
}

static void rna_Sculpt_multi_object_edit_scope_update(bContext *C, PointerRNA *ptr)
{
  const Sculpt *sd = static_cast<const Sculpt *>(ptr->data);
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);

  if (sd->multi_object_edit_scope == SCULPT_MULTI_OBJECT_EDIT_ACTIVE) {
    if (Object *obact = BKE_view_layer_active_object_get(view_layer)) {
      ed::object::object_overlay_mode_transfer_animation_start(C, obact);
    }
    return;
  }

  const ObjectsInModeParams params{OB_MODE_SCULPT, false, nullptr, nullptr};
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_mode_params(
      *bmain, scene, view_layer, CTX_wm_view3d(C), &params);
  for (Object *ob : objects) {
    ed::object::object_overlay_mode_transfer_animation_start(C, ob);
  }
}

static void rna_Sculpt_paint_curve_source_object_update(bContext *C, PointerRNA *ptr)
{
  const Sculpt *sculpt = static_cast<const Sculpt *>(ptr->data);
  if (sculpt->paint_curve_source_object == nullptr) {
    /* User explicitly clicked the clear button: detach from the source and wipe the intermediate
     * paint curve so the Curve Edit tool starts with an empty canvas. */
    ED_paintcurve_detach_source(C);
    return;
  }
  /* ED_paintcurve_import_from_source_object already checks whether the active brush uses the
   * Curve stroke method OR the Curve Edit tool is active, so no additional gate is needed here.
   * The previous check (brush->stroke_method == BRUSH_STROKE_CURVE) wrongly blocked import
   * when using the Curve Edit tool, leaving the picker without an immediate effect. */
  ED_paintcurve_import_from_source_object(C, nullptr, true);
}

static void rna_PaintCurve_use_3d_space_update(bContext *C, PointerRNA *ptr)
{
  PaintCurve *pc = static_cast<PaintCurve *>(ptr->data);
  if (pc == nullptr) {
    return;
  }

  const char use_3d_old = !pc->use_3d_space;

  if (!ED_paintcurve_convert_space_on_toggle(C, pc)) {
    pc->use_3d_space = use_3d_old;
    return;
  }

  WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, nullptr);
}

static void rna_PaintCurve_active_curve_range(
    PointerRNA *ptr, int *min, int *max, int * /*softmin*/, int * /*softmax*/)
{
  const PaintCurve *pc = static_cast<PaintCurve *>(ptr->data);
  *min = 0;
  *max = max_ii(0, pc->geometry.wrap().curves_num() - 1);
}

static int rna_PaintCurve_add_point(PaintCurve *pc, const float position[3], const float radius)
{
  const int index = ED_paintcurve_geometry_add_point(pc, position, radius);
  WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, nullptr);
  return index;
}

static void rna_PaintCurve_points_set(PaintCurve *pc,
                                      ReportList *reports,
                                      const float *positions,
                                      const int positions_num,
                                      const float *radii,
                                      const int radii_num,
                                      const bool cyclic)
{
  if (positions_num % 3 != 0) {
    BKE_report(reports, RPT_ERROR, "Positions must be a flat sequence of XYZ triples");
    return;
  }
  const int points_num = positions_num / 3;
  /* An omitted optional dynamic array arrives as a null pointer, the convention
   * `rna_indices_to_mask()` (`rna_curves_api.cc`) reads too; that is "no radii", not "zero radii".
   */
  const Span<float> radii_span = radii != nullptr ? Span(radii, radii_num) : Span<float>();
  if (!radii_span.is_empty() && radii_span.size() != points_num) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Expected %d radii to match the positions, got %d",
                points_num,
                int(radii_span.size()));
    return;
  }
  ED_paintcurve_geometry_points_set(
      pc, Span(reinterpret_cast<const float3 *>(positions), points_num), radii_span, cyclic);
  WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, nullptr);
}

static void rna_PaintCurve_clear(PaintCurve *pc)
{
  ED_paintcurve_geometry_clear(pc);
  WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, nullptr);
}

/* Everything both read-back functions do before they differ: resolve the brush's parameters,
 * tessellate the curve, optionally snapshot a target surface, and run the core build. No undo step
 * is opened anywhere in here -- neither function writes to the scene.
 *
 * Two kinds of failure are deliberately told apart. MISUSE (no sculpt settings, a target that has
 * no evaluated mesh) reports an error, which the Python layer turns into a `RuntimeError`. A curve
 * that simply has nothing to build from is NOT an error: it returns false silently, the caller
 * returns null, and the script sees `None` -- the same contract `curve_patch_stamps` already has
 * for Ribbon mode, where laying out no stamps is a legitimate answer. */
static bool curve_patch_build_for_rna(bContext *C,
                                      const PaintCurve &pc,
                                      const Brush &brush,
                                      Object *target,
                                      const int spline_index,
                                      const bool use_evaluated,
                                      ReportList *reports,
                                      bke::CurvePatchParams &r_params,
                                      bke::CurvePatchGeometry &r_geometry)
{
  const ToolSettings *tool_settings = CTX_data_tool_settings(C);
  if (tool_settings == nullptr || tool_settings->sculpt == nullptr) {
    BKE_report(reports, RPT_ERROR, "Curve Patch: the scene has no sculpt settings");
    return false;
  }

  /* Sliced out rather than taken whole, and copied rather than mutated in place: a patch is one
   * strip along ONE spline, and reading one must not change the curve it was read from. Taking the
   * geometry whole here used to run the tessellation over every spline at once, quietly welding a
   * multi-spline curve into a single strip. */
  const bke::CurvesGeometry control_curve = ED_paintcurve_control_curve_for_patch(pc,
                                                                                  spline_index);
  if (control_curve.points_num() < 2) {
    /* A spline that short describes no strip. Silent: see the note on failure kinds above. */
    return false;
  }

  r_params = ED_curve_patch_params_from_brush(tool_settings->sculpt->paint, brush, control_curve);

  /* With a target the strip follows that surface, exactly as it does inside a sculpt session; with
   * none it stays in the curve's own plane. The snapshot is built here rather than inside the core
   * build because only this layer knows which mesh the script asked for. */
  if (target != nullptr) {
    const Mesh *mesh = nullptr;
    if (use_evaluated) {
      Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
      const Object *target_eval = DEG_get_evaluated(depsgraph, target);
      mesh = target_eval != nullptr ? BKE_object_get_evaluated_mesh(target_eval) : nullptr;
    }
    else {
      mesh = BKE_object_get_original_mesh(target);
    }
    if (mesh == nullptr) {
      BKE_report(reports, RPT_ERROR, "Curve Patch: the target object has no mesh to follow");
      return false;
    }
    bke::curve_patch_surface_snapshot_build(*mesh, r_geometry.surface);
  }

  /* An empty weight table means "single texture": which slot a stamp would draw changes what the
   * relief SAMPLES, never where the strip runs or where the stamps land. Resolved here so that a
   * read-back reproduces the stamp-to-slot assignment a live session would produce. */
  const Array<float> weights_cdf = ED_curve_patch_stamp_texture_weights_from_brush(
      brush, r_params.radius);
  bke::curve_patch_build_from_control_curve(control_curve,
                                            r_params,
                                            weights_cdf.as_span(),
                                            bke::CurvePatchBuildMode::SurfaceWindowed,
                                            r_geometry);
  if (r_geometry.spline.is_empty()) {
    /* Degenerate input (all points coincident, zero radius). Silent, as above. */
    return false;
  }
  return true;
}

static Mesh *rna_PaintCurve_curve_patch_to_mesh(PaintCurve *pc,
                                                bContext *C,
                                                ReportList *reports,
                                                Brush *brush,
                                                Object *target,
                                                const int spline_index,
                                                const bool use_evaluated)
{
  bke::CurvePatchParams params;
  bke::CurvePatchGeometry geometry;
  if (!curve_patch_build_for_rna(
          C, *pc, *brush, target, spline_index, use_evaluated, reports, params, geometry))
  {
    return nullptr;
  }

  /* The UI stores cap lengths in brush DIAMETERS; the core takes world units. Same conversion as
   * `curve_patch_texture_binding_from_brush()`. */
  const BrushCurvePatchSettings &settings = brush->curve_patch;
  const bool caps_enabled = settings.ribbon_texture_source == BRUSH_CURVE_PATCH_TEX_MULTI;
  Mesh *mesh_nomain = bke::curve_patch_geometry_to_mesh(
      geometry,
      params,
      caps_enabled,
      settings.cap_start_length * 2.0f * params.radius,
      settings.cap_end_length * 2.0f * params.radius);
  if (mesh_nomain == nullptr) {
    /* Nothing to hand back rather than something wrong -- same silent contract as the build. */
    return nullptr;
  }

  Mesh *mesh = BKE_mesh_add(CTX_data_main(C), "CurvePatch");
  BKE_mesh_nomain_to_mesh(mesh_nomain, mesh, nullptr);
  /* Zero users, like every other `bpy.data.*.new()`: the caller decides what links it. */
  id_us_min(&mesh->id);
  WM_main_add_notifier(NC_ID | NA_ADDED, nullptr);
  return mesh;
}

static PointCloud *rna_PaintCurve_curve_patch_stamps(PaintCurve *pc,
                                                     bContext *C,
                                                     ReportList *reports,
                                                     Brush *brush,
                                                     Object *target,
                                                     const int spline_index,
                                                     const bool use_evaluated)
{
  bke::CurvePatchParams params;
  bke::CurvePatchGeometry geometry;
  if (!curve_patch_build_for_rna(
          C, *pc, *brush, target, spline_index, use_evaluated, reports, params, geometry))
  {
    return nullptr;
  }

  PointCloud *points_nomain = bke::curve_patch_geometry_to_stamp_points(geometry);
  if (points_nomain == nullptr) {
    /* Ribbon mode lays out no stamps. Not an error -- the caller asked a question with a
     * legitimate empty answer. */
    return nullptr;
  }

  PointCloud *points = BKE_pointcloud_add(CTX_data_main(C), "CurvePatchStamps");
  BKE_pointcloud_nomain_to_pointcloud(points_nomain, points);
  id_us_min(&points->id);
  WM_main_add_notifier(NC_ID | NA_ADDED, nullptr);
  return points;
}

static void rna_Paint_update(bContext *C, PointerRNA * /*ptr*/)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);

  if (ob) {
    DEG_id_tag_update(&ob->id, ID_RECALC_SHADING);
    WM_main_add_notifier(NC_OBJECT | ND_OB_SHADING, ob);
  }
}

static void rna_Paint_symmetry_space_update(Main * /*bmain*/,
                                            Scene * /*scene*/,
                                            PointerRNA * /*ptr*/)
{
  /* The multi-object sculpt symmetry overlay reads the symmetry space at draw time, so the 3D
   * viewport must redraw when it changes. Its main region does not listen to
   * `NC_SCENE | ND_TOOLSETTINGS` (unlike the tool header), so a view3d-space notifier is sent to
   * refresh the overlay. The tool-settings notifier is still emitted by the property's noteflag.
   */
  WM_main_add_notifier(NC_SPACE | ND_SPACE_VIEW3D, nullptr);
}

static void rna_ImagePaintSettings_gradient_update(Main * /*bmain*/,
                                                   Scene * /*scene*/,
                                                   PointerRNA * /*ptr*/)
{
  /* Invalidate the live gradient preview when any gradient setting (including a ramp stop)
   * changes. */
  ED_image_paint_select_gradient_settings_revision_bump();
}

static void rna_ImagePaintSettings_warp_update(Main * /*bmain*/,
                                               Scene * /*scene*/,
                                               PointerRNA * /*ptr*/)
{
  ED_image_paint_select_warp_settings_revision_bump();
  /* NC_SCENE | ND_TOOLSETTINGS (the notifier this property's RNA_def_property_update() call
   * already sends) only reaches the Image Editor's header/tool-settings *region* listener --
   * the main WINDOW region where the grid is drawn is not tagged, so the visible grid would stay
   * stale until some other event happened to redraw it (e.g. the mouse re-entering the canvas
   * and triggering the paint-cursor hover detector). NC_SPACE | ND_SPACE_IMAGE is handled by the
   * Image Editor's *area* listener instead, which redraws every region -- forcing the WINDOW
   * region to redraw immediately, where draw_select_warp_preview() picks up the bumped revision
   * and resizes the grid before rendering. */
  WM_main_add_notifier(NC_SPACE | ND_SPACE_IMAGE, nullptr);
}

static PointerRNA rna_ImagePaintSettings_gradient_color_ramp_get(PointerRNA *ptr)
{
  ImagePaintSettings *imapaint = static_cast<ImagePaintSettings *>(ptr->data);
  /* NOTE: no lazy initialization here. RNA getters run from UI redraw and Python and must not
   * mutate the data they expose (invisible to undo/depsgraph, and it creates false memfile
   * diffs). The ramp is initialized in #scene_init_data, #blo_update_defaults_scene and
   * #blo_do_versions_520, so it is always valid by the time this getter can be reached. */
  return RNA_pointer_create_with_parent(*ptr, RNA_ColorRamp, &imapaint->gradient_colorband);
}

static std::optional<std::string> rna_Sculpt_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.sculpt";
}

static std::optional<std::string> rna_VertexPaint_path(const PointerRNA *ptr)
{
  const Scene *scene = id_cast<Scene *>(ptr->owner_id);
  const ToolSettings *ts = scene->toolsettings;
  if (ptr->data == ts->vpaint) {
    return "tool_settings.vertex_paint";
  }
  return "tool_settings.weight_paint";
}

static std::optional<std::string> rna_ImagePaintSettings_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.image_paint";
}

static void rna_PaintModeSettings_channel_layer_bindings_begin(CollectionPropertyIterator *iter,
                                                               PointerRNA *ptr)
{
  PaintModeSettings *settings = static_cast<PaintModeSettings *>(ptr->data);
  rna_iterator_array_begin(iter,
                           ptr,
                           settings->channel_layer_bindings,
                           sizeof(MaterialPaintChannelLayerBinding),
                           ARRAY_SIZE(settings->channel_layer_bindings),
                           0,
                           nullptr);
}

/* One binding per channel; the array length is kept in sync with the enum by a static_assert in
 * `paint.cc`. */
static int rna_PaintModeSettings_channel_layer_bindings_length(PointerRNA * /*ptr*/)
{
  return PAINT_MATERIAL_CHANNEL_NUM;
}

/**
 * The bindings are a fixed DNA array on #PaintModeSettings (reached via Scene -> ToolSettings),
 * so a binding's own channel index can only be recovered from where \a ptr points inside that
 * array. Mirrors #rna_BrushMaterialPaintChannel_channel_get (`rna_brush.cc`) exactly, adapted to
 * PaintModeSettings's owner chain instead of Brush's.
 */
static std::optional<eMaterialPaintChannel> rna_MaterialPaintChannelLayerBinding_channel_get(
    const PointerRNA *ptr)
{
  const Scene *scene = reinterpret_cast<const Scene *>(ptr->owner_id);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return std::nullopt;
  }
  const MaterialPaintChannelLayerBinding *bindings =
      scene->toolsettings->paint_mode.channel_layer_bindings;
  const MaterialPaintChannelLayerBinding *binding =
      static_cast<const MaterialPaintChannelLayerBinding *>(ptr->data);
  const int index = int(binding - bindings);
  if (index < 0 || index >= PAINT_MATERIAL_CHANNEL_NUM) {
    return std::nullopt;
  }
  return eMaterialPaintChannel(index);
}

/**
 * Explicit path func for the same reason #rna_BrushMaterialPaintChannel_path exists: the generic
 * ID-to-property path walk cannot address a fixed-array iterator's elements on its own.
 */
static std::optional<std::string> rna_MaterialPaintChannelLayerBinding_path(const PointerRNA *ptr)
{
  const std::optional<eMaterialPaintChannel> channel =
      rna_MaterialPaintChannelLayerBinding_channel_get(ptr);
  if (!channel) {
    return std::nullopt;
  }
  return fmt::format("tool_settings.paint_mode.channel_layer_bindings[{}]", int(*channel));
}

static int rna_MaterialPaintChannelLayerBinding_channel_get_rna(PointerRNA *ptr)
{
  if (const std::optional<eMaterialPaintChannel> channel =
          rna_MaterialPaintChannelLayerBinding_channel_get(ptr))
  {
    return int(*channel);
  }
  return PAINT_MATERIAL_CHANNEL_BASE_COLOR;
}

static void rna_PaintModeSettings_channel_image_bindings_begin(CollectionPropertyIterator *iter,
                                                               PointerRNA *ptr)
{
  PaintModeSettings *settings = static_cast<PaintModeSettings *>(ptr->data);
  rna_iterator_array_begin(iter,
                           ptr,
                           settings->channel_image_bindings,
                           sizeof(MaterialPaintChannelImageBinding),
                           ARRAY_SIZE(settings->channel_image_bindings),
                           0,
                           nullptr);
}

/* See #rna_PaintModeSettings_channel_layer_bindings_length. */
static int rna_PaintModeSettings_channel_image_bindings_length(PointerRNA * /*ptr*/)
{
  return PAINT_MATERIAL_CHANNEL_NUM;
}

/** Mirrors #rna_MaterialPaintChannelLayerBinding_channel_get for the image-binding array. */
static std::optional<eMaterialPaintChannel> rna_MaterialPaintChannelImageBinding_channel_get(
    const PointerRNA *ptr)
{
  const Scene *scene = reinterpret_cast<const Scene *>(ptr->owner_id);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return std::nullopt;
  }
  const MaterialPaintChannelImageBinding *bindings =
      scene->toolsettings->paint_mode.channel_image_bindings;
  const MaterialPaintChannelImageBinding *binding =
      static_cast<const MaterialPaintChannelImageBinding *>(ptr->data);
  const int index = int(binding - bindings);
  if (index < 0 || index >= PAINT_MATERIAL_CHANNEL_NUM) {
    return std::nullopt;
  }
  return eMaterialPaintChannel(index);
}

/** Mirrors #rna_MaterialPaintChannelLayerBinding_path for the image-binding array. */
static std::optional<std::string> rna_MaterialPaintChannelImageBinding_path(const PointerRNA *ptr)
{
  const std::optional<eMaterialPaintChannel> channel =
      rna_MaterialPaintChannelImageBinding_channel_get(ptr);
  if (!channel) {
    return std::nullopt;
  }
  return fmt::format("tool_settings.paint_mode.channel_image_bindings[{}]", int(*channel));
}

static int rna_MaterialPaintChannelImageBinding_channel_get_rna(PointerRNA *ptr)
{
  if (const std::optional<eMaterialPaintChannel> channel =
          rna_MaterialPaintChannelImageBinding_channel_get(ptr))
  {
    return int(*channel);
  }
  return PAINT_MATERIAL_CHANNEL_BASE_COLOR;
}

/**
 * Initializes the binding's own #ImageUser the same way #PaintModeSettings.image_user is set up
 * for #canvas_image: a freshly assigned Image must not be left with a zeroed user (frame 0, no
 * ok flag), or the first stroke resolves no buffer at all.
 */
static void rna_MaterialPaintChannelImageBinding_image_set(PointerRNA *ptr,
                                                           PointerRNA value,
                                                           ReportList * /*reports*/)
{
  MaterialPaintChannelImageBinding *binding = static_cast<MaterialPaintChannelImageBinding *>(
      ptr->data);
  Image *image = static_cast<Image *>(value.data);
  if (binding->image == image) {
    return;
  }
  id_us_min(reinterpret_cast<ID *>(binding->image));
  binding->image = image;
  id_us_plus(reinterpret_cast<ID *>(binding->image));
  BKE_imageuser_default(&binding->iuser);
}

static std::optional<std::string> rna_PaintModeSettings_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.paint_mode";
}

static std::optional<std::string> rna_UvSculpt_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.uv_sculpt";
}

static std::optional<std::string> rna_CurvesSculpt_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.curves_sculpt";
}

static std::optional<std::string> rna_GpPaint_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.gpencil_paint";
}

static std::optional<std::string> rna_GpVertexPaint_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.gpencil_vertex_paint";
}

static std::optional<std::string> rna_GpSculptPaint_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.gpencil_sculpt_paint";
}

static std::optional<std::string> rna_GpWeightPaint_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.gpencil_weight_paint";
}

static std::optional<std::string> rna_ParticleBrush_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.particle_edit.brush";
}

static void rna_ImaPaint_viewport_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA * /*ptr*/)
{
  /* not the best solution maybe, but will refresh the 3D viewport */
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, nullptr);
}

static void rna_ImaPaint_mode_update(bContext *C, PointerRNA * /*ptr*/)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);

  if (ob && ob->type == OB_MESH) {
    /* of course we need to invalidate here */
    BKE_texpaint_slots_refresh_object(scene, ob);

    /* We assume that changing the current mode will invalidate the uv layers
     * so we need to refresh display. */
    ED_paint_proj_mesh_data_check(*scene, *ob, nullptr, nullptr, nullptr, nullptr);
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, nullptr);
  }
}

static void rna_ImaPaint_stencil_update(bContext *C, PointerRNA * /*ptr*/)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);

  if (ob && ob->type == OB_MESH) {
    ED_paint_proj_mesh_data_check(*scene, *ob, nullptr, nullptr, nullptr, nullptr);
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, nullptr);
  }
}

static void rna_ImaPaint_canvas_update(bContext *C, PointerRNA * /*ptr*/)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);
  Image *ima = scene->toolsettings->imapaint.canvas;

  ED_space_image_sync(bmain, ima, false);

  if (ob && ob->type == OB_MESH) {
    ED_paint_proj_mesh_data_check(*scene, *ob, nullptr, nullptr, nullptr, nullptr);
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, nullptr);
  }
}

static void rna_UvSculpt_curve_preset_set(PointerRNA *ptr, int value)
{
  Scene *scene = reinterpret_cast<Scene *>(ptr->owner_id);
  if (value == BRUSH_CURVE_CUSTOM) {
    if (!scene->toolsettings->uvsculpt.curve_distance_falloff) {
      scene->toolsettings->uvsculpt.curve_distance_falloff = BKE_curvemapping_add(
          1, 0.0f, 0.0f, 1.0f, 1.0f);
    }
  }
  scene->toolsettings->uvsculpt.curve_distance_falloff_preset = int8_t(value);
}

/** \name Paint mode settings
 * \{ */

static void rna_PaintModeSettings_canvas_source_update(bContext *C, PointerRNA * /*ptr*/)
{
  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);
  /* When canvas source changes the #pbvh::Tree would require updates when switching between color
   * attributes. */
  if (ob && ob->type == OB_MESH) {
    BKE_texpaint_slots_refresh_object(scene, ob);
    DEG_id_tag_update(&ob->id, 0);
    WM_main_add_notifier(NC_GEOM | ND_DATA, &ob->id);
  }

  /* Switching to the Material canvas is the moment both editors start sharing a target, so align
   * them right away when automatic brush sync is enabled. */
  if (scene != nullptr && scene->toolsettings != nullptr && scene->toolsettings->sculpt != nullptr)
  {
    if (scene->toolsettings->paint_mode.material_paint_flag & PAINT_MATERIAL_BRUSH_SYNC) {
      BKE_paint_material_brush_sync(scene, &scene->toolsettings->sculpt->paint);
    }
  }
}

static void rna_PaintModeSettings_brush_sync_update(bContext *C, PointerRNA * /*ptr*/)
{
  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return;
  }

  /* Align immediately only when enabling. Disabling is deliberately passive: it must not change
   * either editor's PBR settings. */
  ToolSettings *ts = scene->toolsettings;
  const bool use_brush_sync = (ts->paint_mode.material_paint_flag & PAINT_MATERIAL_BRUSH_SYNC) != 0;
  if (!use_brush_sync) {
    BKE_paint_material_brush_sync_disable(CTX_data_main(C), scene);
  }
  else if (ts->sculpt != nullptr) {
    /* Sculpt Mode is the source: PBR Paint is set up there, and the Image Editor is the follower
     * in that workflow. */
    BKE_paint_material_brush_sync(scene, &ts->sculpt->paint);
  }
  WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, scene);
}

static void rna_paint_visible_material_channels_set(PointerRNA *ptr, Paint *paint, int value)
{
  const int added = value & ~paint->visible_material_channels;
  paint->visible_material_channels = value;

  BKE_paint_material_enable_added_visible_channels(*paint, added);
  if (added != 0) {
    WM_main_add_notifier(NC_BRUSH | NA_EDITED, nullptr);
  }

  /* While the two editors share a brush they must also share the channel set, otherwise the
   * follower keeps painting channels the user just hid until the next brush change. */
  Scene *scene = reinterpret_cast<Scene *>(ptr->owner_id);
  Paint *target = scene != nullptr ? BKE_paint_material_sync_target_get(scene, paint) : nullptr;
  if (target != nullptr) {
    target->visible_material_channels = value;
    BKE_paint_material_enable_added_visible_channels(*target, added);
  }
}

static void rna_Sculpt_visible_material_channels_set(PointerRNA *ptr, int value)
{
  Sculpt *sculpt = static_cast<Sculpt *>(ptr->data);
  rna_paint_visible_material_channels_set(ptr, &sculpt->paint, value);
}

static void rna_ImaPaint_visible_material_channels_set(PointerRNA *ptr, int value)
{
  ImagePaintSettings *imapaint = static_cast<ImagePaintSettings *>(ptr->data);
  rna_paint_visible_material_channels_set(ptr, &imapaint->paint, value);
}

/** \} */

static bool rna_ImaPaint_detect_data(ImagePaintSettings *imapaint)
{
  return imapaint->missing_data == 0;
}

static std::optional<std::string> rna_GPencilSculptSettings_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.gpencil_sculpt";
}

static std::optional<std::string> rna_GPencilSculptGuide_path(const PointerRNA * /*ptr*/)
{
  return "tool_settings.gpencil_sculpt.guide";
}

static const MeshAutomaskingSettings *rna_MeshAutomaskingSettings_address_get(const Paint *paint)
{
  if (!paint) {
    return nullptr;
  }

  return paint->mesh_automasking_settings;
}

static std::optional<std::string> rna_MeshAutomaskingSettings_path(const PointerRNA *ptr)
{
  switch (GS(ptr->owner_id->name)) {
    case ID_SCE: {
      const Scene *scene = reinterpret_cast<Scene *>(ptr->owner_id);
      const ToolSettings *tool_settings = scene ? scene->toolsettings : nullptr;
      if (tool_settings == nullptr) {
        return std::nullopt;
      }
      if (rna_MeshAutomaskingSettings_address_get(
              reinterpret_cast<Paint *>(tool_settings->sculpt)) == ptr->data)
      {
        return "tool_settings.sculpt.mesh_automasking_settings";
      }
      return std::nullopt;
    }
    case ID_BR:
      return "mesh_automasking_settings";
    default:
      BLI_assert_unreachable();
      return std::nullopt;
  }
}

static void rna_MeshAutomaskingSettings_invert_cavity_set(PointerRNA *ptr, bool val)
{
  MeshAutomaskingSettings *automasking_settings = static_cast<MeshAutomaskingSettings *>(
      ptr->data);

  if (val) {
    automasking_settings->flags &= ~BRUSH_AUTOMASKING_CAVITY_NORMAL;
    automasking_settings->flags |= BRUSH_AUTOMASKING_CAVITY_INVERTED;
  }
  else {
    automasking_settings->flags &= ~BRUSH_AUTOMASKING_CAVITY_INVERTED;
  }
}

static void rna_MeshAutomaskingSettings_cavity_set(PointerRNA *ptr, bool val)
{
  MeshAutomaskingSettings *automasking_settings = static_cast<MeshAutomaskingSettings *>(
      ptr->data);

  if (val) {
    automasking_settings->flags &= ~BRUSH_AUTOMASKING_CAVITY_INVERTED;
    automasking_settings->flags |= BRUSH_AUTOMASKING_CAVITY_NORMAL;
  }
  else {
    automasking_settings->flags &= ~BRUSH_AUTOMASKING_CAVITY_NORMAL;
  }
}

static void rna_MeshAutomaskingSettings_update(bContext *C, PointerRNA *ptr)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Brush *brush = BKE_paint_brush(BKE_paint_get_active(*bmain, scene, view_layer));

  switch (GS(ptr->owner_id->name)) {
    case ID_BR:
      WM_main_add_notifier(NC_BRUSH | NA_EDITED, brush);
      break;
    case ID_SCE:
      WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, scene);
      break;
    default:
      BLI_assert_unreachable();
  }
}

static void rna_UnifiedPaintSettings_update(bContext *C, PointerRNA *ptr)
{
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Brush *br = BKE_paint_brush(BKE_paint_get_active(*bmain, scene, view_layer));

  /* Every unified setting routes through this callback, so mirroring here covers size, strength,
   * color and jitter in one place - including the radial control modal, which updates through RNA
   * as well. The owning #Paint is found by address: #PointerRNA.data is the settings struct
   * embedded in it. */
  if (scene != nullptr && scene->toolsettings != nullptr) {
    ToolSettings *ts = scene->toolsettings;
    Paint *paints[2] = {ts->sculpt != nullptr ? &ts->sculpt->paint : nullptr, &ts->imapaint.paint};
    for (Paint *paint : paints) {
      if (paint != nullptr && &paint->unified_paint_settings == ptr->data) {
        BKE_paint_material_unified_settings_sync(scene, paint);
        break;
      }
    }
  }

  /* TODO: Verify if tagging the brush for these settings being changed is correct. */
  WM_main_add_notifier(NC_BRUSH | NA_EDITED, br);
  WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, scene);
}

static void rna_UnifiedPaintSettings_color_update(bContext *C, PointerRNA *ptr)
{
  UnifiedPaintSettings *ups = static_cast<UnifiedPaintSettings *>(ptr->data);
  rna_UnifiedPaintSettings_update(C, ptr);
  BKE_brush_color_sync_legacy(ups);

  /* Sync Base Color channel from active brush color when Sync with Brush is on. */
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Paint *paint = BKE_paint_get_active(*bmain, scene, view_layer);
  Brush *brush = BKE_paint_brush(paint);
  BKE_brush_material_paint_base_color_sync_to_channel(paint, brush);
  WM_main_add_notifier(NC_BRUSH | NA_EDITED, brush);
}

static void rna_UnifiedPaintSettings_size_set(PointerRNA *ptr, int value)
{
  UnifiedPaintSettings *ups = static_cast<UnifiedPaintSettings *>(ptr->data);

  /* scale unprojected size so it stays consistent with brush size */
  BKE_brush_scale_unprojected_size(&ups->unprojected_size, value, ups->size);
  ups->size = value;
}

static void rna_UnifiedPaintSettings_unprojected_size_set(PointerRNA *ptr, float value)
{
  UnifiedPaintSettings *ups = static_cast<UnifiedPaintSettings *>(ptr->data);

  /* scale brush size so it stays consistent with unprojected_size */
  BKE_brush_scale_size(&ups->size, value, ups->unprojected_size);
  ups->unprojected_size = value;
}

static void rna_UnifiedPaintSettings_size_update(bContext *C, PointerRNA *ptr)
{
  /* changing the unified size should invalidate the overlay but also update the brush */
  BKE_paint_invalidate_overlay_all();
  rna_UnifiedPaintSettings_update(C, ptr);
}

static const UnifiedPaintSettings *rna_UnifiedPaintSettings_address_get(const Paint *paint)
{
  if (!paint) {
    return nullptr;
  }

  return &paint->unified_paint_settings;
}

static std::optional<std::string> rna_UnifiedPaintSettings_path(const PointerRNA *ptr)
{
  const Scene *scene = reinterpret_cast<Scene *>(ptr->owner_id);
  const ToolSettings *tool_settings = scene ? scene->toolsettings : nullptr;
  if (tool_settings == nullptr) {
    return std::nullopt;
  }
  if (rna_UnifiedPaintSettings_address_get(reinterpret_cast<Paint *>(tool_settings->vpaint)) ==
      ptr->data)
  {
    return "tool_settings.vertex_paint.unified_paint_settings";
  }
  if (rna_UnifiedPaintSettings_address_get(reinterpret_cast<Paint *>(tool_settings->wpaint)) ==
      ptr->data)
  {
    return "tool_settings.weight_paint.unified_paint_settings";
  }
  if (rna_UnifiedPaintSettings_address_get(reinterpret_cast<Paint *>(tool_settings->sculpt)) ==
      ptr->data)
  {
    return "tool_settings.sculpt.unified_paint_settings";
  }
  if (rna_UnifiedPaintSettings_address_get(reinterpret_cast<Paint *>(tool_settings->gp_paint)) ==
      ptr->data)
  {
    return "tool_settings.gpencil_paint.unified_paint_settings";
  }
  if (rna_UnifiedPaintSettings_address_get(
          reinterpret_cast<Paint *>(tool_settings->gp_vertexpaint)) == ptr->data)
  {
    return "tool_settings.gpencil_vertex_paint.unified_paint_settings";
  }
  if (rna_UnifiedPaintSettings_address_get(
          reinterpret_cast<Paint *>(tool_settings->gp_sculptpaint)) == ptr->data)
  {
    return "tool_settings.gpencil_sculpt_paint.unified_paint_settings";
  }
  if (rna_UnifiedPaintSettings_address_get(
          reinterpret_cast<Paint *>(tool_settings->gp_weightpaint)) == ptr->data)
  {
    return "tool_settings.gpencil_weight_paint.unified_paint_settings";
  }
  if (rna_UnifiedPaintSettings_address_get(
          reinterpret_cast<Paint *>(tool_settings->curves_sculpt)) == ptr->data)
  {
    return "tool_settings.curves_sculpt.unified_paint_settings";
  }
  return std::nullopt;
}

static float rna_Paint_mirror_snap_distance_get(PointerRNA *ptr)
{
  const Paint *paint = static_cast<const Paint *>(ptr->data);
  return BKE_paint_mirror_snap_distance_get(*paint);
}

static void rna_Paint_mirror_snap_distance_set(PointerRNA *ptr, const float value)
{
  Paint *paint = static_cast<Paint *>(ptr->data);
  /* Clamp here, not just via #RNA_def_property_range: a custom setter bypasses that, so a Python
   * assignment could otherwise store a value the UI cannot express -- and a value <= 0 would be
   * read back as the default by #BKE_paint_mirror_snap_distance_get. */
  paint->mirror_snap_distance = clamp_f(value, 1.0f, 10.0f);
}

static bool rna_Paint_use_override_face_sets_get(PointerRNA *ptr)
{
  const Paint *paint = static_cast<const Paint *>(ptr->data);
  return paint->runtime != nullptr && paint->runtime->override_face_sets;
}

static void rna_Paint_use_override_face_sets_set(PointerRNA *ptr, bool value)
{
  Paint *paint = static_cast<Paint *>(ptr->data);
  if (paint->runtime != nullptr) {
    paint->runtime->override_face_sets = value;
  }
}

static bool rna_Paint_use_override_stroke_get(PointerRNA *ptr)
{
  const Paint *paint = static_cast<const Paint *>(ptr->data);
  return paint->runtime != nullptr && paint->runtime->override_stroke;
}

static void rna_Paint_use_override_stroke_set(PointerRNA *ptr, bool value)
{
  Paint *paint = static_cast<Paint *>(ptr->data);
  if (paint->runtime != nullptr) {
    paint->runtime->override_stroke = value;
  }
}

static bool rna_Paint_use_override_falloff_get(PointerRNA *ptr)
{
  const Paint *paint = static_cast<const Paint *>(ptr->data);
  return paint->runtime != nullptr && paint->runtime->override_falloff;
}

static void rna_Paint_use_override_falloff_set(PointerRNA *ptr, bool value)
{
  Paint *paint = static_cast<Paint *>(ptr->data);
  if (paint->runtime != nullptr) {
    paint->runtime->override_falloff = value;
  }
}

}  // namespace blender

#else

namespace blender {

static void rna_def_paint_curve(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;
  FunctionRNA *func;
  PropertyRNA *parm;

  srna = RNA_def_struct(brna, "PaintCurve", "ID");
  RNA_def_struct_ui_text(srna, "Paint Curve", "");
  RNA_def_struct_ui_icon(srna, ICON_CURVE_BEZCURVE);

  prop = RNA_def_property(srna, "use_3d_space", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_3d_space", 1);
  RNA_def_property_ui_text(
      prop, "3D Space", "Store and edit curve in 3D object space instead of 2D screen space");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_PaintCurve_use_3d_space_update");

  prop = RNA_def_property(srna, "show_radius_handles", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "show_radius_handles", 1);
  RNA_def_property_ui_text(
      prop, "Radius Handles", "Show draggable radius handles at each control point");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  /* Control points and splines. The collection callbacks are the ones `Curves` uses: both ID types
   * embed a #blender::bke::CurvesGeometry, and the callbacks resolve it from the owner ID rather
   * than assuming one of the two. */

  prop = RNA_def_property(srna, "points", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "CurvePoint");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_IGNORE);
  RNA_def_property_collection_funcs(prop,
                                    "rna_Curves_position_data_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    "rna_Curves_position_data_length",
                                    "rna_Curves_points_lookup_int",
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(prop, "Points", "Control points of all splines");

  /* Raw access to the position array, for `foreach_get`/`foreach_set`. `points[i].position` writes
   * one point at a time and runs the bezier handle recompute after EACH of them, which is O(N^2)
   * over a whole curve; `pyrna` calls the update of this collection exactly once, at the end of
   * the `foreach_set` (`bpy_rna.cc`), so the same write costs one recompute in total.
   *
   * The trade-off is deliberate: this path writes positions straight into the array and does NOT
   * carry the bezier handles along with their points the way `CurvePoint.position` does. AUTO and
   * ALIGNED handles are rebuilt by the update below, FREE ones stay where they were. */
  prop = RNA_def_property(srna, "position_data", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "FloatVectorAttributeValue");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_IGNORE);
  RNA_def_property_collection_funcs(prop,
                                    "rna_Curves_position_data_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    "rna_Curves_position_data_length",
                                    "rna_Curves_position_data_lookup_int",
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(
      prop, "Position Data", "Control point positions, for batch access from a script");
  RNA_def_property_update(prop, 0, "rna_curve_geometry_update_data");

  prop = RNA_def_property(srna, "curves", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "CurveSlice");
  RNA_def_property_override_flag(prop, PROPOVERRIDE_IGNORE);
  RNA_def_property_collection_funcs(prop,
                                    "rna_Curves_curves_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    "rna_Curves_curves_length",
                                    "rna_Curves_curves_lookup_int",
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(prop, "Splines", "All splines of this paint curve");

  prop = RNA_def_property(srna, "active_curve", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "active_curve");
  RNA_def_property_int_funcs(prop, nullptr, nullptr, "rna_PaintCurve_active_curve_range");
  RNA_def_property_ui_text(prop, "Active Spline", "Index of the spline edit operations act on");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  /* Building a curve from scratch. The paint curve operators cannot stand in for these: they need
   * a region and a mouse position, so they are unavailable in background mode. */

  func = RNA_def_function(srna, "add_point", "rna_PaintCurve_add_point");
  RNA_def_function_ui_description(func, "Append a control point to the active spline");
  parm = RNA_def_float_vector(func,
                              "position",
                              3,
                              nullptr,
                              -FLT_MAX,
                              FLT_MAX,
                              "Position",
                              "Control point position in object space",
                              -FLT_MAX,
                              FLT_MAX);
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_float(func,
                "radius",
                1.0f,
                0.0f,
                FLT_MAX,
                "Radius",
                "Radius as a fraction of the brush size, where 1.0 is the full size",
                0.0f,
                100.0f);
  parm = RNA_def_int(
      func, "index", 0, -1, INT_MAX, "Index", "Index of the new control point", -1, INT_MAX);
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "points_set", "rna_PaintCurve_points_set");
  RNA_def_function_ui_description(
      func,
      "Replace every spline with a single spline built from the given positions, in one pass. "
      "Cheaper than calling add_point() in a loop, which recomputes the bezier handles of the "
      "whole curve on every call");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_float_array(func,
                             "positions",
                             1,
                             nullptr,
                             -FLT_MAX,
                             FLT_MAX,
                             "Positions",
                             "Flat sequence of XYZ control point positions in object space",
                             -FLT_MAX,
                             FLT_MAX);
  RNA_def_parameter_flags(parm, PROP_DYNAMIC, PARM_REQUIRED);
  parm = RNA_def_float_array(func,
                             "radii",
                             1,
                             nullptr,
                             0.0f,
                             FLT_MAX,
                             "Radii",
                             "One radius per point as a fraction of the brush size, where 1.0 is "
                             "the full size; every point gets 1.0 when omitted",
                             0.0f,
                             100.0f);
  RNA_def_parameter_flags(parm, PROP_DYNAMIC, ParameterFlag(0));
  RNA_def_boolean(func, "cyclic", false, "Cyclic", "Close the spline into a loop");

  func = RNA_def_function(srna, "clear", "rna_PaintCurve_clear");
  RNA_def_function_ui_description(func, "Remove all control points and splines");

  /* Reading a Curve Patch back out. Neither call stamps anything or opens an undo step; both need
   * the context only to reach the sculpt settings the brush's size is unified through, and the
   * depsgraph a target object is evaluated in.
   *
   * Both answer None when the curve describes nothing to build, and raise only on misuse -- a
   * scene without sculpt settings, or a target that has no evaluated mesh. */

  func = RNA_def_function(srna, "curve_patch_to_mesh", "rna_PaintCurve_curve_patch_to_mesh");
  RNA_def_function_ui_description(
      func, "Build the Curve Patch ribbon this curve describes as a new mesh with UVs");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT | FUNC_USE_REPORTS);
  parm = RNA_def_pointer(
      func, "brush", "Brush", "", "Brush supplying the Curve Patch settings and size");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_pointer(
      func,
      "target",
      "Object",
      "",
      "Object whose evaluated surface the ribbon is laid onto; when omitted the "
      "ribbon stays in the curve's own plane");
  RNA_def_int(func,
              "spline_index",
              -1,
              -1,
              INT_MAX,
              "Spline",
              "Which spline of the curve to build, or -1 for the curve's active spline. Reading "
              "is always single-spline: a patch is one strip along one spline, and welding "
              "several into a single mesh is exactly what this call avoids. Use "
              "sculpt.curve_patch_apply(use_all_splines=True) to stamp every spline at once",
              -1,
              INT_MAX);
  RNA_def_boolean(func,
                  "use_evaluated",
                  true,
                  "Use Evaluated",
                  "Follow the target's evaluated surface, with modifiers applied. Disable to "
                  "follow the original mesh, which is what a sculpt session stamps onto -- note "
                  "that on Multires a session applies no surface snapshot at all, so the two "
                  "cannot be made to agree there");
  parm = RNA_def_pointer(func,
                         "mesh",
                         "Mesh",
                         "",
                         "The ribbon, or None when the spline has fewer than two points or is "
                         "otherwise degenerate");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "curve_patch_stamps", "rna_PaintCurve_curve_patch_stamps");
  RNA_def_function_ui_description(
      func,
      "Build the Curve Patch stamp layout this curve describes as a new point cloud, one point "
      "per stamp");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT | FUNC_USE_REPORTS);
  parm = RNA_def_pointer(
      func, "brush", "Brush", "", "Brush supplying the Curve Patch settings and size");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_pointer(func,
                         "target",
                         "Object",
                         "",
                         "Object whose evaluated surface the stamps are laid onto; when omitted "
                         "they stay in the curve's own plane");
  RNA_def_int(func,
              "spline_index",
              -1,
              -1,
              INT_MAX,
              "Spline",
              "Which spline of the curve to build, or -1 for the curve's active spline. Reading "
              "is always single-spline: a patch is one strip along one spline, and welding "
              "several into a single layout is exactly what this call avoids. Use "
              "sculpt.curve_patch_apply(use_all_splines=True) to stamp every spline at once",
              -1,
              INT_MAX);
  RNA_def_boolean(func,
                  "use_evaluated",
                  true,
                  "Use Evaluated",
                  "Follow the target's evaluated surface, with modifiers applied. Disable to "
                  "follow the original mesh, which is what a sculpt session stamps onto -- note "
                  "that on Multires a session applies no surface snapshot at all, so the two "
                  "cannot be made to agree there");
  parm = RNA_def_pointer(
      func, "points", "PointCloud", "", "The stamps, or None in Ribbon mode, which lays out none");
  RNA_def_function_return(func, parm);

  rna_def_attributes_common(srna, AttributeOwnerType::PaintCurve);
}

static void rna_def_paint_curve_visibility_flag(StructRNA *srna,
                                                const char *prop_name,
                                                const char *ui_name,
                                                const int64_t flag)
{
  PropertyRNA *prop;

  prop = RNA_def_property(srna, prop_name, PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "curve_visibility_flags", flag);
  RNA_def_property_ui_text(prop, ui_name, nullptr);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);
}

/**
 * Define #Paint.visible_material_channels on \a srna, reached through \a sdna_path.
 *
 * Deliberately not part of #rna_def_paint: only Sculpt Mode and Image Paint paint PBR material
 * channels, and exposing the property on every #Paint sub-type (Vertex/Weight/Grease Pencil/Curves
 * Sculpt) would add API surface that is neither versioned nor meaningful there.
 */
static void rna_def_paint_visible_material_channels(StructRNA *srna,
                                                    const char *sdna_path,
                                                    const char *set_func)
{
  PropertyRNA *prop = RNA_def_enum_flag(
      srna,
      "visible_material_channels",
      rna_enum_visible_material_paint_channel_items,
      PAINT_MATERIAL_CHANNELS_VISIBLE_DEFAULT,
      "Visible Channels",
      "Which material paint channels are shown in the PBR Paint channel list and painted during "
      "strokes; hidden channels keep their settings but are skipped until shown again");
  RNA_def_property_enum_sdna(prop, nullptr, sdna_path);
  RNA_def_property_enum_funcs(prop, nullptr, set_func, nullptr);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);
}

static void rna_def_paint(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static const EnumPropertyItem symmetry_space_items[] = {
      {PAINT_SYMM_SPACE_ACTIVE_OBJECT,
       "ACTIVE_OBJECT",
       0,
       "Object",
       "Mirror across the active object's own local axes (default; matches the same meshes after "
       "joining them)"},
      {PAINT_SYMM_SPACE_GLOBAL_WORLD,
       "GLOBAL_WORLD",
       0,
       "World",
       "Mirror across world axes through the scene world origin"},
      {PAINT_SYMM_SPACE_GLOBAL_CURSOR,
       "GLOBAL_CURSOR",
       0,
       "Cursor",
       "Mirror across world axes through the 3D cursor"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  srna = RNA_def_struct(brna, "Paint", nullptr);
  RNA_def_struct_ui_text(srna, "Paint", "");

  /* Global Settings */
  prop = RNA_def_property(srna, "brush", PROP_POINTER, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_struct_type(prop, "Brush");
  RNA_def_property_pointer_funcs(
      prop, "rna_Paint_brush_get", nullptr, nullptr, "rna_Paint_brush_poll");
  RNA_def_property_ui_text(prop, "Brush", "Active brush");
  RNA_def_property_update(prop, NC_BRUSH | NA_SELECTED, nullptr);

  prop = RNA_def_property(srna, "brush_asset_reference", PROP_POINTER, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "Brush Asset Reference",
                           "A weak reference to the matching brush asset, used e.g. to restore "
                           "the last used brush on file load");

  prop = RNA_def_property(srna, "palette", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_pointer_funcs(prop, nullptr, nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Palette", "Active Palette");

  prop = RNA_def_property(srna, "show_brush", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flags", PAINT_SHOW_BRUSH);
  RNA_def_property_ui_text(prop, "Show Brush", "");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_override_face_sets", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_Paint_use_override_face_sets_get", "rna_Paint_use_override_face_sets_set");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop,
                           "Override Face Sets",
                           "Apply the active brush's Face Sets settings to every brush activated "
                           "during this session");
  RNA_def_property_update(prop, NC_BRUSH | NA_EDITED, nullptr);

  prop = RNA_def_property(srna, "use_override_stroke", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_Paint_use_override_stroke_get", "rna_Paint_use_override_stroke_set");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop,
                           "Override Stroke",
                           "Apply the active brush's Stroke settings to every brush activated "
                           "during this session");
  RNA_def_property_update(prop, NC_BRUSH | NA_EDITED, nullptr);

  prop = RNA_def_property(srna, "use_override_falloff", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_Paint_use_override_falloff_get", "rna_Paint_use_override_falloff_set");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop,
                           "Override Falloff",
                           "Apply the active brush's Falloff settings to every brush activated "
                           "during this session");
  RNA_def_property_update(prop, NC_BRUSH | NA_EDITED, nullptr);

  prop = RNA_def_property(srna, "show_brush_on_surface", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flags", PAINT_SHOW_BRUSH_ON_SURFACE);
  RNA_def_property_ui_text(prop, "Show Brush On Surface", "");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "show_low_resolution", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flags", PAINT_FAST_NAVIGATE);
  RNA_def_property_ui_text(
      prop, "Fast Navigate", "For multires, show low resolution while navigating the view");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_sculpt_delay_updates", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flags", PAINT_SCULPT_DELAY_UPDATES);
  RNA_def_property_ui_text(
      prop,
      "Delay Viewport Updates",
      "Update the geometry when it enters the view, providing faster view navigation");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "show_bvh_nodes", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "debug_flags", PAINT_DEBUG_SHOW_BVH_NODES);
  RNA_def_property_ui_text(
      prop, "Show BVH Nodes", "Show the underlying BVH nodes as differently colored faces");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Paint_update");

  prop = RNA_def_property(srna, "use_symmetry_x", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "symmetry_flags", PAINT_SYMM_X);
  RNA_def_property_ui_text(prop, "Symmetry X", "Mirror brush across the X axis");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_symmetry_y", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "symmetry_flags", PAINT_SYMM_Y);
  RNA_def_property_ui_text(prop, "Symmetry Y", "Mirror brush across the Y axis");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_symmetry_z", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "symmetry_flags", PAINT_SYMM_Z);
  RNA_def_property_ui_text(prop, "Symmetry Z", "Mirror brush across the Z axis");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_symmetry_feather", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "symmetry_flags", PAINT_SYMMETRY_FEATHER);
  RNA_def_property_ui_text(prop,
                           "Symmetry Feathering",
                           "Reduce the strength of the brush where it overlaps symmetrical daubs");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "symmetry_space", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "symmetry_space");
  RNA_def_property_enum_items(prop, symmetry_space_items);
  RNA_def_property_ui_text(
      prop,
      "Symmetry Space",
      "Space of the brush symmetry plane in multi-object sculpt: the active object's local axes, "
      "or world axes pivoted at the world origin or 3D cursor");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_Paint_symmetry_space_update");

  prop = RNA_def_property(srna, "use_mirror_surface_snap", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(
      prop, nullptr, "symmetry_flags", PAINT_SYMMETRY_MIRROR_SNAP_OFF);
  RNA_def_property_ui_text(prop,
                           "Snap Mirror to Surface",
                           "In multi-object sculpt, pull a mirrored brush daub onto the nearest "
                           "surface of the mirrored object, so Sphere falloff still reaches it "
                           "when that object is not an exact mirror of the sculpted one");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "mirror_snap_distance", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_funcs(
      prop, "rna_Paint_mirror_snap_distance_get", "rna_Paint_mirror_snap_distance_set", nullptr);
  RNA_def_property_range(prop, 1.0f, 10.0f);
  RNA_def_property_ui_range(prop, 1.0f, 4.0f, 0.1f, 2);
  /* Default comes from DNA (#Paint.mirror_snap_distance = 2.0f); legacy files store 0 in the
   * former padding and #BKE_paint_mirror_snap_distance_get maps that back to 2.0f at runtime. */
  RNA_def_property_ui_text(prop,
                           "Snap Distance",
                           "How far a mirrored brush daub may travel along the surface normal to "
                           "reach the mirrored object, in brush radii. Beyond this the object is "
                           "left untouched rather than partially dented");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  /* Deprecated: superseded by #symmetry_space (PAINT_SYMM_SPACE_ACTIVE_OBJECT is now always-on for
   * multi-object strokes). Kept as a no-op so existing Python scripts do not break; the underlying
   * #PAINT_SYMMETRY_SHARED_ORIGIN bit is no longer read. */
  prop = RNA_def_property(srna, "use_symmetry_shared_origin", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "symmetry_flags", PAINT_SYMMETRY_SHARED_ORIGIN);
  RNA_def_property_ui_text(
      prop, "Shared Symmetry Origin", "Deprecated, use symmetry_space instead");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "cavity_curve", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_ui_text(prop, "Curve", "Editable cavity curve");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_cavity", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flags", PAINT_USE_CAVITY_MASK);
  RNA_def_property_ui_text(prop, "Cavity Mask", "Mask painting according to mesh geometry cavity");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "tile_offset", PROP_FLOAT, PROP_XYZ_LENGTH);
  RNA_def_property_float_sdna(prop, nullptr, "tile_offset");
  RNA_def_property_array(prop, 3);
  RNA_def_property_range(prop, 0.01, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.01, 100, 1 * 100, 2);
  RNA_def_property_ui_text(
      prop, "Tiling offset for the X Axis", "Stride at which tiled strokes are copied");

  prop = RNA_def_property(srna, "tile_x", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "symmetry_flags", PAINT_TILE_X);
  RNA_def_property_ui_text(prop, "Tile X", "Tile along X axis");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "tile_y", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "symmetry_flags", PAINT_TILE_Y);
  RNA_def_property_ui_text(prop, "Tile Y", "Tile along Y axis");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "tile_z", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "symmetry_flags", PAINT_TILE_Z);
  RNA_def_property_ui_text(prop, "Tile Z", "Tile along Z axis");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  rna_def_paint_curve_visibility_flag(
      srna, "show_strength_curve", "Show Strength Curve", PAINT_CURVE_SHOW_STRENGTH);
  rna_def_paint_curve_visibility_flag(
      srna, "show_size_curve", "Show Size Curve", PAINT_CURVE_SHOW_SIZE);
  rna_def_paint_curve_visibility_flag(
      srna, "show_jitter_curve", "Show Jitter Curve", PAINT_CURVE_SHOW_JITTER);

  /* Unified Paint Settings */
  prop = RNA_def_property(srna, "unified_paint_settings", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "UnifiedPaintSettings");
  RNA_def_property_ui_text(prop, "Unified Paint Settings", nullptr);

  prop = RNA_def_property(srna, "mesh_automasking_settings", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "MeshAutomaskingSettings");
  RNA_def_property_ui_text(prop, "Mesh Automasking Settings", nullptr);
}

static void rna_def_unified_paint_settings(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static const EnumPropertyItem brush_size_unit_items[] = {
      {0, "VIEW", 0, "View", "Measure brush size relative to the view"},
      {UNIFIED_PAINT_BRUSH_LOCK_SIZE,
       "SCENE",
       0,
       "Scene",
       "Measure brush size relative to the scene"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  srna = RNA_def_struct(brna, "UnifiedPaintSettings", nullptr);
  RNA_def_struct_path_func(srna, "rna_UnifiedPaintSettings_path");
  RNA_def_struct_ui_text(
      srna, "Unified Paint Settings", "Overrides for some of the active brush's settings");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);

  /* high-level flags to enable or disable unified paint settings */
  prop = RNA_def_property(srna, "use_unified_size", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", UNIFIED_PAINT_SIZE);
  RNA_def_property_ui_text(
      prop, "Use Unified Size", "Instead of per-brush size, the size is shared across brushes");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_unified_strength", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", UNIFIED_PAINT_ALPHA);
  RNA_def_property_ui_text(prop,
                           "Use Unified Strength",
                           "Instead of per-brush strength, the strength is shared across brushes");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_unified_weight", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", UNIFIED_PAINT_WEIGHT);
  RNA_def_property_ui_text(prop,
                           "Use Unified Weight",
                           "Instead of per-brush weight, the weight is shared across brushes");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_unified_color", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", UNIFIED_PAINT_COLOR);
  RNA_def_property_ui_text(
      prop, "Use Unified Color", "Instead of per-brush color, the color is shared across brushes");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_unified_input_samples", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", UNIFIED_PAINT_INPUT_SAMPLES);
  RNA_def_property_ui_text(
      prop,
      "Use Unified Input Samples",
      "Instead of per-brush input samples, the value is shared across brushes");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  /* unified paint settings that override the equivalent settings
   * from the active brush */
  prop = RNA_def_property(srna, "size", PROP_INT, PROP_PIXEL_DIAMETER);
  RNA_def_property_int_funcs(prop, nullptr, "rna_UnifiedPaintSettings_size_set", nullptr);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_range(prop, 1, MAX_BRUSH_PIXEL_DIAMETER * 10);
  RNA_def_property_ui_range(prop, 1, MAX_BRUSH_PIXEL_DIAMETER, 1, -1);
  RNA_def_property_ui_text(prop, "Size", "Diameter of the brush");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_size_update");

  prop = RNA_def_property(srna, "unprojected_size", PROP_FLOAT, PROP_DISTANCE_DIAMETER);
  RNA_def_property_float_funcs(
      prop, nullptr, "rna_UnifiedPaintSettings_unprojected_size_set", nullptr);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_range(prop, 0.001, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.001, 1, 1, -1);
  RNA_def_property_ui_text(prop, "Unprojected Size", "Diameter of brush in Blender units");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_size_update");

  prop = RNA_def_property(srna, "strength", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "alpha");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_range(prop, 0.0f, 10.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(
      prop, "Strength", "How powerful the effect of the brush is when applied");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "weight", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "weight");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.001, 3);
  RNA_def_property_ui_text(prop, "Weight", "Weight to assign in vertex groups");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_float_sdna(prop, nullptr, "color");
  RNA_def_property_ui_text(prop, "Color", "");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_color_update");

  prop = RNA_def_property(srna, "secondary_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_float_sdna(prop, nullptr, "secondary_color");
  RNA_def_property_ui_text(prop, "Secondary Color", "");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_color_update");

  prop = RNA_def_property(srna, "use_color_jitter", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", UNIFIED_PAINT_COLOR_JITTER);
  RNA_def_property_ui_text(prop, "Use Color Jitter", "Jitter brush color");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "hue_jitter", PROP_FLOAT, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_float_sdna(prop, nullptr, "hsv_jitter[0]");
  RNA_def_property_range(prop, 0, 1.0f);
  RNA_def_property_ui_range(prop, 0, 1, 0.05, 2);
  RNA_def_property_ui_text(prop, "Hue Jitter", "Color jitter effect on hue");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "saturation_jitter", PROP_FLOAT, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_float_sdna(prop, nullptr, "hsv_jitter[1]");
  RNA_def_property_range(prop, 0, 1.0f);
  RNA_def_property_ui_range(prop, 0, 1, 0.05, 2);
  RNA_def_property_ui_text(prop, "Saturation Jitter", "Color jitter effect on saturation");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "value_jitter", PROP_FLOAT, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_float_sdna(prop, nullptr, "hsv_jitter[2]");
  RNA_def_property_range(prop, 0, 1.0f);
  RNA_def_property_ui_range(prop, 0, 1, 0.05, 2);
  RNA_def_property_ui_text(prop, "Value Jitter", "Color jitter effect on value");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "use_stroke_random_hue", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "color_jitter_flag", BRUSH_COLOR_JITTER_USE_HUE_AT_STROKE);
  RNA_def_property_ui_icon(prop, ICON_GP_SELECT_STROKES, 0);
  RNA_def_property_ui_text(prop, "Stroke Random", "Use randomness at stroke level");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "use_stroke_random_sat", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "color_jitter_flag", BRUSH_COLOR_JITTER_USE_SAT_AT_STROKE);
  RNA_def_property_ui_icon(prop, ICON_GP_SELECT_STROKES, 0);
  RNA_def_property_ui_text(prop, "Stroke Random", "Use randomness at stroke level");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "use_stroke_random_val", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "color_jitter_flag", BRUSH_COLOR_JITTER_USE_VAL_AT_STROKE);
  RNA_def_property_ui_icon(prop, ICON_GP_SELECT_STROKES, 0);
  RNA_def_property_ui_text(prop, "Stroke Random", "Use randomness at stroke level");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "use_random_press_hue", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "color_jitter_flag", BRUSH_COLOR_JITTER_USE_HUE_RAND_PRESS);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(prop, "Use Pressure", "Use pressure to modulate randomness");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "use_random_press_sat", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "color_jitter_flag", BRUSH_COLOR_JITTER_USE_SAT_RAND_PRESS);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(prop, "Use Pressure", "Use pressure to modulate randomness");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "use_random_press_val", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "color_jitter_flag", BRUSH_COLOR_JITTER_USE_VAL_RAND_PRESS);
  RNA_def_property_ui_icon(prop, ICON_STYLUS_PRESSURE, 0);
  RNA_def_property_ui_text(prop, "Use Pressure", "Use pressure to modulate randomness");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "input_samples", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_int_sdna(prop, nullptr, "input_samples");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_range(prop, 1, PAINT_MAX_INPUT_SAMPLES);
  RNA_def_property_ui_range(prop, 1, PAINT_MAX_INPUT_SAMPLES, 1, -1);
  RNA_def_property_ui_text(
      prop,
      "Input Samples",
      "Number of input samples to average together to smooth the brush stroke");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");

  prop = RNA_def_property(srna, "use_locked_size", PROP_ENUM, PROP_NONE); /* as an enum */
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "flag");
  RNA_def_property_enum_items(prop, brush_size_unit_items);
  RNA_def_property_ui_text(
      prop, "Size Unit", "Measure brush size relative to the view or the scene");
  RNA_def_property_update(prop, 0, "rna_UnifiedPaintSettings_update");
}

static void rna_def_mesh_automasking_settings(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "MeshAutomaskingSettings", nullptr);
  RNA_def_struct_path_func(srna, "rna_MeshAutomaskingSettings_path");
  RNA_def_struct_ui_text(
      srna, "Automasking Settings", "Automasking settings for mesh painting & sculpting.");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);

  const EnumPropertyItem *entry = rna_enum_shared_automasking_flag_items;
  do {
    prop = RNA_def_property(srna, entry->identifier, PROP_BOOLEAN, PROP_NONE);
    RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
    RNA_def_property_boolean_sdna(prop, nullptr, "flags", entry->value);
    RNA_def_property_ui_text(prop, entry->name, entry->description);

    if (entry->value == BRUSH_AUTOMASKING_CAVITY_NORMAL) {
      RNA_def_property_boolean_funcs(prop, nullptr, "rna_MeshAutomaskingSettings_cavity_set");
    }
    else if (entry->value == BRUSH_AUTOMASKING_CAVITY_INVERTED) {
      RNA_def_property_boolean_funcs(
          prop, nullptr, "rna_MeshAutomaskingSettings_invert_cavity_set");
    }

    RNA_def_property_update(prop, 0, "rna_MeshAutomaskingSettings_update");
  } while ((++entry)->identifier);

  prop = RNA_def_property(srna, "boundary_edges_propagation_steps", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_int_sdna(prop, nullptr, "boundary_edges_propagation_steps");
  RNA_def_property_range(prop, 1, AUTOMASKING_BOUNDARY_EDGES_MAX_PROPAGATION_STEPS);
  RNA_def_property_ui_range(prop, 1, AUTOMASKING_BOUNDARY_EDGES_MAX_PROPAGATION_STEPS, 1, -1);
  RNA_def_property_ui_text(prop,
                           "Propagation Steps",
                           "Distance where boundary edge automasking is going to protect vertices "
                           "from the fully masked edge");
  RNA_def_property_update(prop, 0, "rna_MeshAutomaskingSettings_update");

  prop = RNA_def_property(srna, "cavity_factor", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_float_sdna(prop, nullptr, "cavity_factor");
  RNA_def_property_ui_text(prop, "Cavity Factor", "The contrast of the cavity mask");
  RNA_def_property_float_default(prop, 1.0f);
  RNA_def_property_range(prop, 0.0f, 5.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.1, 3);
  RNA_def_property_update(prop, 0, "rna_MeshAutomaskingSettings_update");

  prop = RNA_def_property(srna, "cavity_blur_steps", PROP_INT, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_int_sdna(prop, nullptr, "cavity_blur_steps");
  RNA_def_property_ui_text(prop, "Blur Steps", "The number of times the cavity mask is blurred");
  RNA_def_property_int_default(prop, 0);
  RNA_def_property_range(prop, 0, 25);
  RNA_def_property_ui_range(prop, 0, 10, 1, 1);
  RNA_def_property_update(prop, 0, "rna_MeshAutomaskingSettings_update");

  prop = RNA_def_property(srna, "cavity_curve", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_pointer_sdna(prop, nullptr, "cavity_curve");
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_ui_text(prop, "Cavity Curve", "Curve used for the sensitivity");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_MeshAutomaskingSettings_update");

  prop = RNA_def_property(srna, "cavity_curve_op", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_pointer_sdna(prop, nullptr, "cavity_curve_op");
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_ui_text(prop, "Cavity Curve", "Curve used for the sensitivity");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_MeshAutomaskingSettings_update");

  prop = RNA_def_property(srna, "start_normal_limit", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_float_sdna(prop, nullptr, "start_normal_limit");
  RNA_def_property_range(prop, 0.0001f, M_PI);
  RNA_def_property_ui_text(prop, "Area Normal Limit", "The range of angles that will be affected");
  RNA_def_property_update(prop, 0, "rna_MeshAutomaskingSettings_update");

  prop = RNA_def_property(srna, "start_normal_falloff", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_float_sdna(prop, nullptr, "start_normal_falloff");
  RNA_def_property_range(prop, 0.0001f, 1.0f);
  RNA_def_property_ui_text(
      prop, "Area Normal Falloff", "Extend the angular range with a falloff gradient");
  RNA_def_property_update(prop, 0, "rna_MeshAutomaskingSettings_update");

  prop = RNA_def_property(srna, "view_normal_limit", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_float_sdna(prop, nullptr, "view_normal_limit");
  RNA_def_property_range(prop, 0.0001f, M_PI);
  RNA_def_property_ui_text(prop, "View Normal Limit", "The range of angles that will be affected");
  RNA_def_property_update(prop, 0, "rna_MeshAutomaskingSettings_update");

  prop = RNA_def_property(srna, "view_normal_falloff", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_float_sdna(prop, nullptr, "view_normal_falloff");
  RNA_def_property_range(prop, 0.0001f, 1.0f);
  RNA_def_property_ui_text(
      prop, "View Normal Falloff", "Extend the angular range with a falloff gradient");
  RNA_def_property_update(prop, 0, "rna_MeshAutomaskingSettings_update");
}

static void rna_def_sculpt(BlenderRNA *brna)
{
  static const EnumPropertyItem detail_refine_items[] = {
      {SCULPT_DYNTOPO_SUBDIVIDE,
       "SUBDIVIDE",
       0,
       "Subdivide Edges",
       "Subdivide long edges to add mesh detail where needed"},
      {SCULPT_DYNTOPO_COLLAPSE,
       "COLLAPSE",
       0,
       "Collapse Edges",
       "Collapse short edges to remove mesh detail where possible"},
      {SCULPT_DYNTOPO_SUBDIVIDE | SCULPT_DYNTOPO_COLLAPSE,
       "SUBDIVIDE_COLLAPSE",
       0,
       "Subdivide Collapse",
       "Both subdivide long edges and collapse short edges to refine mesh detail"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem detail_type_items[] = {
      {0,
       "RELATIVE",
       0,
       "Relative Detail",
       "Mesh detail is relative to the brush size and detail size"},
      {SCULPT_DYNTOPO_DETAIL_CONSTANT,
       "CONSTANT",
       0,
       "Constant Detail",
       "Mesh detail is constant in world space according to detail size"},
      {SCULPT_DYNTOPO_DETAIL_BRUSH,
       "BRUSH",
       0,
       "Brush Detail",
       "Mesh detail is relative to brush size"},
      {SCULPT_DYNTOPO_DETAIL_MANUAL,
       "MANUAL",
       0,
       "Manual Detail",
       "Mesh detail does not change on each stroke, only when using Flood Fill"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem sculpt_transform_mode_items[] = {
      {SCULPT_TRANSFORM_MODE_ALL_VERTICES,
       "ALL_VERTICES",
       0,
       "All Vertices",
       "Applies the transformation to all vertices in the mesh"},
      {SCULPT_TRANSFORM_MODE_RADIUS_ELASTIC,
       "RADIUS_ELASTIC",
       0,
       "Elastic",
       "Applies the transformation simulating elasticity using the radius of the cursor"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem sculpt_multi_object_edit_scope_items[] = {
      {SCULPT_MULTI_OBJECT_EDIT_ACTIVE,
       "ACTIVE",
       0,
       "Active Object",
       "Brush strokes and tools only affect the active object"},
      {SCULPT_MULTI_OBJECT_EDIT_ALL,
       "ALL",
       0,
       "All Objects",
       "Brush strokes and tools affect every object currently in Sculpt Mode"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "Sculpt", "Paint");
  RNA_def_struct_path_func(srna, "rna_Sculpt_path");
  RNA_def_struct_ui_text(srna, "Sculpt", "");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);

  rna_def_paint_visible_material_channels(
      srna, "paint.visible_material_channels", "rna_Sculpt_visible_material_channels_set");

  prop = RNA_def_property(srna, "lock_x", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flags", SCULPT_LOCK_X);
  RNA_def_property_ui_text(prop, "Lock X", "Disallow changes to the X axis of vertices");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "lock_y", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flags", SCULPT_LOCK_Y);
  RNA_def_property_ui_text(prop, "Lock Y", "Disallow changes to the Y axis of vertices");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "lock_z", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flags", SCULPT_LOCK_Z);
  RNA_def_property_ui_text(prop, "Lock Z", "Disallow changes to the Z axis of vertices");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "show_layer_preview", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flags", SCULPT_SHOW_LAYER_PREVIEW);
  RNA_def_property_ui_text(prop,
                           "Show Layer Preview",
                           "Tint the surface where the active sculpt layer holds recorded "
                           "displacement");
  RNA_def_property_update(prop, NC_SPACE | ND_SPACE_VIEW3D, nullptr);

  prop = RNA_def_property(srna, "layer_preview_threshold", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "sculpt_layer_preview_threshold");
  RNA_def_property_range(prop, 0.0001f, 1.0f);
  RNA_def_property_ui_range(prop, 0.001f, 0.25f, 0.01, 4);
  RNA_def_property_ui_text(prop,
                           "Layer Preview Threshold",
                           "Displacement at which the layer preview reaches full tint, as a "
                           "fraction of the mesh bounding box diagonal");
  RNA_def_property_update(prop, NC_SPACE | ND_SPACE_VIEW3D, nullptr);

  prop = RNA_def_property(srna, "layer_preview_opacity", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "sculpt_layer_preview_opacity");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Layer Preview Opacity", "");
  RNA_def_property_update(prop, NC_SPACE | ND_SPACE_VIEW3D, nullptr);

  prop = RNA_def_property(srna, "use_deform_only", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flags", SCULPT_ONLY_DEFORM);
  RNA_def_property_ui_text(prop,
                           "Use Deform Only",
                           "Use only deformation modifiers (temporary disable all "
                           "constructive modifiers except multi-resolution)");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Sculpt_update");

  prop = RNA_def_property(srna, "detail_size", PROP_FLOAT, PROP_PIXEL);
  RNA_def_property_range(prop, 0.5, 40.0);
  RNA_def_property_ui_range(prop, 0.5, 40.0, 0.1, 2);
  RNA_def_property_ui_scale_type(prop, PROP_SCALE_CUBIC);
  RNA_def_property_ui_text(
      prop, "Detail Size", "Maximum edge length for dynamic topology sculpting (in pixels)");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "detail_percent", PROP_FLOAT, PROP_PERCENTAGE);
  RNA_def_property_range(prop, 0.5, 100.0);
  RNA_def_property_ui_range(prop, 0.5, 100.0, 10, 2);
  RNA_def_property_ui_text(
      prop,
      "Detail Percentage",
      "Maximum edge length for dynamic topology sculpting (in brush percentage)");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "constant_detail_resolution", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "constant_detail");
  RNA_def_property_range(prop, 0.0001, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.001, 1000.0, 10, 2);
  RNA_def_property_ui_text(prop,
                           "Resolution",
                           "Maximum edge length for dynamic topology sculpting (as divisor "
                           "of Blender unit - higher value means smaller edge length)");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "symmetrize_direction", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_symmetrize_direction_items);
  RNA_def_property_ui_text(prop, "Direction", "Source and destination for symmetrize operator");

  prop = RNA_def_property(srna, "detail_refine_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "flags");
  RNA_def_property_enum_items(prop, detail_refine_items);
  RNA_def_property_ui_text(
      prop, "Detail Refine Method", "In dynamic-topology mode, how to add or remove mesh detail");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "detail_type_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "flags");
  RNA_def_property_enum_items(prop, detail_type_items);
  RNA_def_property_ui_text(
      prop, "Detail Type Method", "In dynamic-topology mode, how mesh detail size is calculated");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "gravity", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "gravity_factor");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.1, 3);
  RNA_def_property_ui_text(prop, "Gravity", "Amount of gravity after each dab");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "transform_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, sculpt_transform_mode_items);
  RNA_def_property_ui_text(
      prop, "Transform Mode", "How the transformation is going to be applied to the target");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "multi_object_edit_scope", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, sculpt_multi_object_edit_scope_items);
  RNA_def_property_ui_text(
      prop,
      "Multi-Object Edit Scope",
      "Whether brush strokes and tools act on the active object only, or on every object "
      "currently in Sculpt Mode");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(
      prop, NC_SCENE | ND_TOOLSETTINGS, "rna_Sculpt_multi_object_edit_scope_update");

  prop = RNA_def_property(srna, "transform_all_objects", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "transform_all_objects", false);
  RNA_def_property_boolean_default(prop, false);
  RNA_def_property_ui_text(
      prop,
      "Affect All Objects",
      "Move, rotate, and scale every object currently in Sculpt Mode together, around one "
      "shared pivot, instead of only the active object");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "transform_origin_correct", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "transform_origin_correct", false);
  RNA_def_property_boolean_default(prop, false);
  RNA_def_property_ui_text(
      prop,
      "Correct Origin",
      "Move non-active objects' own origin together with their mesh, like an Object Mode "
      "transform, instead of leaving it in place. Only applies with \"Affect All Objects\" "
      "enabled and Transform Mode set to \"All Vertices\"");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "gravity_object", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(
      prop, "Orientation", "Object whose Z axis defines orientation of gravity");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "paint_curve_source_object", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_pointer_funcs(
      prop, nullptr, nullptr, nullptr, "rna_PaintCurve_source_object_poll");
  RNA_def_property_ui_text(
      prop, "Source Curve", "Curves or Curve object to import into the active paint curve");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(
      prop, NC_SCENE | ND_TOOLSETTINGS, "rna_Sculpt_paint_curve_source_object_update");

  prop = RNA_def_property(srna, "paint_curve_sync_to_source", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "paint_curve_sync_to_source", 0);
  RNA_def_property_ui_text(
      prop,
      "Sync to Source Curve",
      "Live update of the picked source object while editing the paint curve");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "paint_curve_show_radius_handles", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "paint_curve_show_radius_handles", 1);
  RNA_def_property_ui_text(
      prop, "Show Radius Handles", "Display radius handles for paint curve points");
  RNA_def_property_update(prop, NC_SPACE | ND_SPACE_VIEW3D, nullptr);

  static const EnumPropertyItem radius_display_items[] = {
      {SCULPT_PAINT_CURVE_RADIUS_ALL, "ALL", 0, "All", "Show radius handles for all points"},
      {SCULPT_PAINT_CURVE_RADIUS_SELECT,
       "SELECT",
       0,
       "Selected",
       "Show radius handles only for curves with selected points"},
      {SCULPT_PAINT_CURVE_RADIUS_TIPS,
       "TIPS",
       0,
       "Tips",
       "Show radius handles only at start and end points"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  prop = RNA_def_property(srna, "paint_curve_radius_display_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "paint_curve_radius_display_mode");
  RNA_def_property_enum_items(prop, radius_display_items);
  RNA_def_property_ui_text(prop, "Radius Display Mode", "Which radius handles to display");
  RNA_def_property_update(prop, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
}

static void rna_def_uv_sculpt(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "UvSculpt", nullptr);
  RNA_def_struct_path_func(srna, "rna_UvSculpt_path");
  RNA_def_struct_ui_text(srna, "UV Sculpting", "");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);

  prop = RNA_def_property(srna, "size", PROP_INT, PROP_PIXEL_DIAMETER);
  RNA_def_property_ui_range(prop, 1, MAX_BRUSH_PIXEL_DIAMETER, 1, 1);
  RNA_def_property_range(prop, 1, MAX_BRUSH_PIXEL_DIAMETER * 10);
  RNA_def_property_ui_text(prop, "Size", "");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "strength", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Strength", "");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_AMOUNT);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "curve_distance_falloff", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_pointer_funcs(prop, nullptr, nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Falloff Curve", "");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "curve_distance_falloff_preset", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_brush_curve_preset_items);
  RNA_def_property_ui_text(prop, "Falloff Curve Preset", "");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_CURVE_LEGACY);
  RNA_def_property_enum_funcs(prop, nullptr, "rna_UvSculpt_curve_preset_set", nullptr);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);
}

static void rna_def_gp_paint(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "GpPaint", "Paint");
  RNA_def_struct_path_func(srna, "rna_GpPaint_path");
  RNA_def_struct_ui_text(srna, "Grease Pencil Paint", "");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);

  /* Use vertex color (main switch). */
  prop = RNA_def_property(srna, "color_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "mode");
  RNA_def_property_enum_items(prop, rna_enum_gpencil_paint_mode);
  RNA_def_property_ui_text(prop, "Mode", "Paint Mode");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
}

static void rna_def_gp_vertexpaint(BlenderRNA *brna)
{
  StructRNA *srna;

  srna = RNA_def_struct(brna, "GpVertexPaint", "Paint");
  RNA_def_struct_path_func(srna, "rna_GpVertexPaint_path");
  RNA_def_struct_ui_text(srna, "Grease Pencil Vertex Paint", "");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);
}

static void rna_def_gp_sculptpaint(BlenderRNA *brna)
{
  StructRNA *srna;

  srna = RNA_def_struct(brna, "GpSculptPaint", "Paint");
  RNA_def_struct_path_func(srna, "rna_GpSculptPaint_path");
  RNA_def_struct_ui_text(srna, "Grease Pencil Sculpt Paint", "");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);
}

static void rna_def_gp_weightpaint(BlenderRNA *brna)
{
  StructRNA *srna;

  srna = RNA_def_struct(brna, "GpWeightPaint", "Paint");
  RNA_def_struct_path_func(srna, "rna_GpWeightPaint_path");
  RNA_def_struct_ui_text(srna, "Grease Pencil Weight Paint", "");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);
}

/* use for weight paint too */
static void rna_def_vertex_paint(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "VertexPaint", "Paint");
  RNA_def_struct_sdna(srna, "VPaint");
  RNA_def_struct_path_func(srna, "rna_VertexPaint_path");
  RNA_def_struct_ui_text(srna, "Vertex Paint", "Properties of vertex and weight paint mode");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);

  /* weight paint only */
  prop = RNA_def_property(srna, "use_group_restrict", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", VP_FLAG_VGROUP_RESTRICT);
  RNA_def_property_ui_text(prop, "Restrict", "Restrict painting to vertices in the group");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);
}

static void rna_def_paint_mode(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "MaterialPaintChannelLayerBinding", nullptr);
  RNA_def_struct_sdna(srna, "MaterialPaintChannelLayerBinding");
  RNA_def_struct_path_func(srna, "rna_MaterialPaintChannelLayerBinding_path");
  RNA_def_struct_ui_text(srna,
                         "Material Paint Channel Layer Binding",
                         "Attribute a material paint channel writes into: an add-on managed "
                         "override for a fixed channel, or the user-configured attribute name "
                         "for the Custom channel");
  /* Same reason #PaintModeSettings clears it: tool settings are not undo-tracked, and a nested
   * struct that still pushes undo steps would make its owner's behavior inconsistent. */
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);

  /* Read-only, mirroring #BrushMaterialPaintChannel.channel: the bindings are a fixed array, so
   * this is what lets Python (and add-ons) key them by channel instead of hard-coding indices
   * that would silently drift if a channel is added or reordered. */
  prop = RNA_def_property(srna, "channel", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_channel_items);
  RNA_def_property_enum_funcs(
      prop, "rna_MaterialPaintChannelLayerBinding_channel_get_rna", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Channel", "Which material paint channel this binding redirects");

  prop = RNA_def_property(srna, "attribute_name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, nullptr, "attribute_name");
  RNA_def_property_ui_text(
      prop,
      "Attribute Name",
      "Name of the mesh attribute this channel paints into. For a fixed channel, empty means "
      "paint its built-in attribute as normal; for Custom, empty means unconfigured. Takes "
      "effect for the next stroke, not necessarily mid-stroke");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  srna = RNA_def_struct(brna, "MaterialPaintChannelImageBinding", nullptr);
  RNA_def_struct_sdna(srna, "MaterialPaintChannelImageBinding");
  RNA_def_struct_path_func(srna, "rna_MaterialPaintChannelImageBinding_path");
  RNA_def_struct_ui_text(srna,
                         "Material Paint Channel Image Binding",
                         "Add-on managed Image a material paint channel writes into on the "
                         "image/texture canvas, instead of whatever the Principled BSDF socket "
                         "resolves to");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);

  /* Read-only; see #MaterialPaintChannelLayerBinding.channel. */
  prop = RNA_def_property(srna, "channel", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_material_paint_channel_items);
  RNA_def_property_enum_funcs(
      prop, "rna_MaterialPaintChannelImageBinding_channel_get_rna", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Channel", "Which material paint channel this binding redirects");

  prop = RNA_def_property(srna, "image", PROP_POINTER, PROP_NONE);
  /* An explicit setter rather than the generic one: the binding owns an #ImageUser that has to be
   * reset alongside the pointer, and the reference has to be counted so the Image is not freed
   * while a channel still paints into it. */
  RNA_def_property_pointer_funcs(prop,
                                 nullptr,
                                 "rna_MaterialPaintChannelImageBinding_image_set",
                                 nullptr,
                                 "rna_Image_no_renderresult_or_viewer_poll");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_ID_REFCOUNT);
  RNA_def_property_ui_text(
      prop,
      "Image",
      "Image this channel paints into on the PAINT_CANVAS_SOURCE_MATERIAL canvas; empty to "
      "resolve it from the active material's node tree as normal. Takes effect for the next "
      "stroke, not necessarily mid-stroke");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  srna = RNA_def_struct(brna, "PaintModeSettings", nullptr);
  RNA_def_struct_sdna(srna, "PaintModeSettings");
  RNA_def_struct_path_func(srna, "rna_PaintModeSettings_path");
  RNA_def_struct_ui_text(srna, "Paint Mode", "Properties of paint mode");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);

  prop = RNA_def_property(srna, "canvas_source", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_canvas_source_items);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_ui_text(prop, "Source", "Source to select canvas from");
  RNA_def_property_update(prop, 0, "rna_PaintModeSettings_canvas_source_update");

  prop = RNA_def_property(srna, "use_brush_sync", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "material_paint_flag", PAINT_MATERIAL_BRUSH_SYNC);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_ui_text(prop,
                           "Sync Brush",
                           "Use the same brush and brush settings in Sculpt Mode and the Image "
                           "Editor while painting the Material canvas");
  RNA_def_property_update(prop, 0, "rna_PaintModeSettings_brush_sync_update");

  prop = RNA_def_property(srna, "canvas_image", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_funcs(
      prop, nullptr, nullptr, nullptr, "rna_Image_no_renderresult_or_viewer_poll");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_CONTEXT_UPDATE);
  RNA_def_property_ui_text(prop, "Texture", "Image used as painting target");

  /* Custom channel value range stays scene-level; enable/value/blend live on
   * #Brush.material_paint. Custom's attribute name is exposed via #channel_layer_bindings below,
   * the same collection every other channel's redirect/name uses. */
  prop = RNA_def_property(srna, "channel_custom_range", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "channel_custom_range");
  RNA_def_property_array(prop, 2);
  RNA_def_property_ui_range(prop, -10000.0f, 10000.0f, 0.01, 3);
  RNA_def_property_ui_text(
      prop, "Custom Range", "Range painted values of the custom channel are clamped to");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "channel_layer_bindings", PROP_COLLECTION, PROP_NONE);
  /* Empty length name: fixed DNA array (same pattern as BrushMaterialPaint.channels). */
  RNA_def_property_collection_sdna(prop, nullptr, "channel_layer_bindings", "");
  RNA_def_property_struct_type(prop, "MaterialPaintChannelLayerBinding");
  RNA_def_property_collection_funcs(prop,
                                    "rna_PaintModeSettings_channel_layer_bindings_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    "rna_PaintModeSettings_channel_layer_bindings_length",
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(
      prop,
      "Channel Layer Bindings",
      "Per-channel material paint attribute redirect, indexed by material paint channel");

  prop = RNA_def_property(srna, "channel_image_bindings", PROP_COLLECTION, PROP_NONE);
  /* Empty length name: fixed DNA array (same pattern as channel_layer_bindings above). */
  RNA_def_property_collection_sdna(prop, nullptr, "channel_image_bindings", "");
  RNA_def_property_struct_type(prop, "MaterialPaintChannelImageBinding");
  RNA_def_property_collection_funcs(prop,
                                    "rna_PaintModeSettings_channel_image_bindings_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_get",
                                    "rna_PaintModeSettings_channel_image_bindings_length",
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(
      prop,
      "Channel Image Bindings",
      "Per-channel material paint image redirect for the image/texture canvas, indexed by "
      "material paint channel");

  static const EnumPropertyItem new_channel_image_size_items[] = {
      {PAINT_NEW_CHANNEL_IMAGE_SIZE_256, "SIZE_256", 0, "256 (256 x 256)", "256 x 256"},
      {PAINT_NEW_CHANNEL_IMAGE_SIZE_512, "SIZE_512", 0, "512 (512 x 512)", "512 x 512"},
      {PAINT_NEW_CHANNEL_IMAGE_SIZE_1K, "SIZE_1K", 0, "1K (1024 x 1024)", "1024 x 1024"},
      {PAINT_NEW_CHANNEL_IMAGE_SIZE_2K, "SIZE_2K", 0, "2K (2048 x 2048)", "2048 x 2048"},
      {PAINT_NEW_CHANNEL_IMAGE_SIZE_4K, "SIZE_4K", 0, "4K (4096 x 4096)", "4096 x 4096"},
      {PAINT_NEW_CHANNEL_IMAGE_SIZE_8K, "SIZE_8K", 0, "8K (8192 x 8192)", "8192 x 8192"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  prop = RNA_def_property(srna, "new_channel_image_size", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "new_channel_image_size");
  RNA_def_property_enum_items(prop, new_channel_image_size_items);
  RNA_def_property_enum_default(prop, PAINT_NEW_CHANNEL_IMAGE_SIZE_4K);
  RNA_def_property_ui_text(prop,
                           "New Channel Image Size",
                           "Width and height used for material paint channel images that "
                           "are auto-created when a channel is enabled");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_enum_flag(srna,
                           "material_shader_visible_channels",
                           rna_enum_visible_material_paint_channel_items,
                           PAINT_MATERIAL_CHANNELS_SHADER_VISIBLE_DEFAULT,
                           "Shader Visible Channels",
                           "Which material paint channels the Material Paint (vertex color) "
                           "canvas displays in the 3D Viewport, independent of whether the "
                           "channel is enabled for painting or shown in the PBR Paint list: a "
                           "channel keeps its painted data and can be hidden from shading, or "
                           "shown, without either affecting painting");
  RNA_def_property_enum_sdna(prop, nullptr, "material_shader_visible_channels");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);
}

static void rna_def_image_paint(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;
  FunctionRNA *func;

  static const EnumPropertyItem paint_type_items[] = {
      {IMAGEPAINT_MODE_MATERIAL,
       "MATERIAL",
       0,
       "Material",
       "Detect image slots from the material"},
      {IMAGEPAINT_MODE_IMAGE,
       "IMAGE",
       0,
       "Single Image",
       "Set image for texture painting directly"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem paint_interp_items[] = {
      {IMAGEPAINT_INTERP_LINEAR, "LINEAR", 0, "Linear", "Linear interpolation"},
      {IMAGEPAINT_INTERP_CLOSEST,
       "CLOSEST",
       0,
       "Closest",
       "No interpolation (sample closest texel)"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  srna = RNA_def_struct(brna, "ImagePaint", "Paint");
  RNA_def_struct_sdna(srna, "ImagePaintSettings");
  RNA_def_struct_path_func(srna, "rna_ImagePaintSettings_path");
  RNA_def_struct_ui_text(srna, "Image Paint", "Properties of image and texture painting mode");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);

  rna_def_paint_visible_material_channels(
      srna, "paint.visible_material_channels", "rna_ImaPaint_visible_material_channels_set");

  /* functions */
  func = RNA_def_function(srna, "detect_data", "rna_ImaPaint_detect_data");
  RNA_def_function_ui_description(func, "Check if required texpaint data exist");

  /* return type */
  RNA_def_function_return(func, RNA_def_boolean(func, "ok", true, "", ""));

  /* booleans */
  prop = RNA_def_property(srna, "use_occlude", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(prop, nullptr, "flag", IMAGEPAINT_PROJECT_XRAY);
  RNA_def_property_ui_text(
      prop, "Occlude", "Only paint onto the faces directly under the brush (slower)");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_backface_culling", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(prop, nullptr, "flag", IMAGEPAINT_PROJECT_BACKFACE);
  RNA_def_property_ui_text(prop, "Cull", "Ignore faces pointing away from the view (faster)");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_normal_falloff", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_negative_sdna(prop, nullptr, "flag", IMAGEPAINT_PROJECT_FLAT);
  RNA_def_property_ui_text(prop, "Normal", "Paint most on faces pointing towards the view");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_stencil_layer", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", IMAGEPAINT_PROJECT_LAYER_STENCIL);
  RNA_def_property_ui_text(prop, "Stencil Layer", "Set the mask layer from the UV map buttons");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImaPaint_viewport_update");

  prop = RNA_def_property(srna, "invert_stencil", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", IMAGEPAINT_PROJECT_LAYER_STENCIL_INV);
  RNA_def_property_ui_text(prop, "Invert", "Invert the stencil layer");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImaPaint_viewport_update");

  prop = RNA_def_property(srna, "stencil_image", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "stencil");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_CONTEXT_UPDATE);
  RNA_def_property_ui_text(prop, "Stencil Image", "Image used as stencil");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImaPaint_stencil_update");
  RNA_def_property_pointer_funcs(
      prop, nullptr, nullptr, nullptr, "rna_Image_no_renderresult_or_viewer_poll");

  prop = RNA_def_property(srna, "canvas", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_CONTEXT_UPDATE);
  RNA_def_property_ui_text(prop, "Canvas", "Image used as canvas");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImaPaint_canvas_update");
  RNA_def_property_pointer_funcs(
      prop, nullptr, nullptr, nullptr, "rna_Image_no_renderresult_or_viewer_poll");

  prop = RNA_def_property(srna, "clone_image", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "clone");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Clone Image", "Image used as clone source");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  RNA_def_property_pointer_funcs(
      prop, nullptr, nullptr, nullptr, "rna_Image_no_renderresult_or_viewer_poll");

  prop = RNA_def_property(srna, "stencil_color", PROP_FLOAT, PROP_COLOR_GAMMA);
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_float_sdna(prop, nullptr, "stencil_col");
  RNA_def_property_ui_text(prop, "Stencil Color", "Stencil color in the viewport");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImaPaint_viewport_update");

  prop = RNA_def_property(srna, "dither", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 2.0);
  RNA_def_property_ui_text(prop, "Dither", "Amount of dithering when painting on byte images");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_clone_layer", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", IMAGEPAINT_PROJECT_LAYER_CLONE);
  RNA_def_property_ui_text(
      prop,
      "Clone Map",
      "Use another UV map as clone source, otherwise use the 3D cursor as the source");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImaPaint_viewport_update");

  /* integers */

  prop = RNA_def_property(srna, "seam_bleed", PROP_INT, PROP_PIXEL);
  RNA_def_property_ui_range(prop, 0, 8, 1, -1);
  RNA_def_property_ui_text(
      prop, "Bleed", "Extend paint beyond the faces' UVs to reduce seams (in pixels, slower)");

  prop = RNA_def_property(srna, "normal_angle", PROP_INT, PROP_UNSIGNED);
  RNA_def_property_range(prop, 0, 90);
  RNA_def_property_ui_text(
      prop, "Angle", "Paint most on faces pointing towards the view according to this angle");

  prop = RNA_def_int_array(srna,
                           "screen_grab_size",
                           2,
                           nullptr,
                           0,
                           0,
                           "Screen Grab Size",
                           "Size to capture the image for re-projecting",
                           0,
                           0);
  RNA_def_property_range(prop, 512, 16384);
  RNA_def_property_subtype(prop, PROP_PIXEL);

  prop = RNA_def_property(srna, "mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_enum_items(prop, paint_type_items);
  RNA_def_property_ui_text(prop, "Mode", "Mode of operation for projection painting");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImaPaint_mode_update");

  prop = RNA_def_property(srna, "interpolation", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "interp");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_enum_items(prop, paint_interp_items);
  RNA_def_property_ui_text(prop, "Interpolation", "Texture filtering type");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImaPaint_mode_update");

  /* Missing data */
  prop = RNA_def_property(srna, "missing_uvs", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "missing_data", IMAGEPAINT_MISSING_UVS);
  RNA_def_property_ui_text(prop, "Missing UVs", "A UV layer is missing on the mesh");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  prop = RNA_def_property(srna, "missing_materials", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "missing_data", IMAGEPAINT_MISSING_MATERIAL);
  RNA_def_property_ui_text(prop, "Missing Materials", "The mesh is missing materials");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  prop = RNA_def_property(srna, "missing_stencil", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "missing_data", IMAGEPAINT_MISSING_STENCIL);
  RNA_def_property_ui_text(prop, "Missing Stencil", "Image Painting does not have a stencil");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  prop = RNA_def_property(srna, "missing_texture", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "missing_data", IMAGEPAINT_MISSING_TEX);
  RNA_def_property_ui_text(
      prop, "Missing Texture", "Image Painting does not have a texture to paint on");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  prop = RNA_def_property(srna, "clone_alpha", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "clone_alpha");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Clone Alpha", "Opacity of clone image display");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "clone_offset", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, nullptr, "clone_offset");
  RNA_def_property_ui_text(prop, "Clone Offset", "");
  RNA_def_property_ui_range(prop, -1.0f, 1.0f, 10.0f, 3);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  /* NOTE: there is deliberately no `use_selection_mask` property. Whether selection masking is
   * active is derived from the per-image runtime mask data, which #ImagePaintSettings cannot
   * reach; operator polls query #BKE_image_paint_selection_mask_has_any instead. */

  static const EnumPropertyItem selection_expand_items[] = {
      {IMAGE_PAINT_SELECT_EXPAND_PIXELS,
       "PIXELS",
       ICON_IMAGE,
       "Pixels",
       "Select the exact pixels covered by the gesture"},
      {IMAGE_PAINT_SELECT_EXPAND_FACE,
       "FACE",
       ICON_UV_FACESEL,
       "Face",
       "Expand the selection to entire faces that overlap the gesture"},
      {IMAGE_PAINT_SELECT_EXPAND_ISLAND,
       "ISLAND",
       ICON_UV_ISLANDSEL,
       "Island",
       "Expand the selection to entire UV islands that overlap the gesture"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  prop = RNA_def_property(srna, "selection_expand", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "selection_expand");
  RNA_def_property_enum_items(prop, selection_expand_items);
  RNA_def_property_ui_text(
      prop, "Selection Expand", "How a paint selection gesture is expanded onto the UV layout");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "warp_grid_size", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "warp_grid_size");
  RNA_def_property_range(prop, 2, 10);
  RNA_def_property_ui_text(
      prop, "Grid Size", "Number of control points along each side of the Warp selection grid");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImagePaintSettings_warp_update");

  static const EnumPropertyItem warp_interpolation_items[] = {
      {IMAGE_PAINT_WARP_INTERP_LINEAR,
       "LINEAR",
       0,
       "Linear",
       "Piecewise bilinear deformation; fast, but creases along the grid lines"},
      {IMAGE_PAINT_WARP_INTERP_SMOOTH,
       "SMOOTH",
       0,
       "Smooth",
       "Bicubic (Catmull-Rom) deformation with continuous curvature across the grid lines"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  prop = RNA_def_property(srna, "warp_interpolation", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "warp_interpolation");
  RNA_def_property_enum_items(prop, warp_interpolation_items);
  RNA_def_property_ui_text(
      prop, "Interpolation", "Interpolation used to deform the Warp selection grid");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImagePaintSettings_warp_update");

  static const EnumPropertyItem gradient_type_items[] = {
      {IMAGE_PAINT_GRADIENT_LINEAR, "LINEAR", 0, "Linear", "Interpolate along the gradient line"},
      {IMAGE_PAINT_GRADIENT_RADIAL,
       "RADIAL",
       0,
       "Radial",
       "Interpolate radially from the start point"},
      {IMAGE_PAINT_GRADIENT_CONICAL,
       "CONICAL",
       0,
       "Conical",
       "Interpolate by angle around the start point"},
      {IMAGE_PAINT_GRADIENT_DIAMOND,
       "DIAMOND",
       0,
       "Diamond",
       "Interpolate along diamond-shaped iso-lines around the start point"},
      {IMAGE_PAINT_GRADIENT_SQUARE,
       "SQUARE",
       0,
       "Square",
       "Interpolate along square-shaped iso-lines around the start point"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem gradient_repeat_items[] = {
      {IMAGE_PAINT_GRADIENT_REPEAT_NONE, "NONE", 0, "None", ""},
      {IMAGE_PAINT_GRADIENT_REPEAT_REPEAT, "REPEAT", 0, "Repeat", ""},
      {IMAGE_PAINT_GRADIENT_REPEAT_REFLECT, "REFLECT", 0, "Reflect", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem gradient_blend_items[] = {
      {IMB_BLEND_MIX, "MIX", 0, "Mix", ""},
      {IMB_BLEND_MUL, "MUL", 0, "Multiply", ""},
      {IMB_BLEND_ADD, "ADD", 0, "Add", ""},
      {IMB_BLEND_SUB, "SUB", 0, "Subtract", ""},
      {IMB_BLEND_OVERLAY, "OVERLAY", 0, "Overlay", ""},
      {IMB_BLEND_SCREEN, "SCREEN", 0, "Screen", ""},
      {IMB_BLEND_DARKEN, "DARKEN", 0, "Darken", ""},
      {IMB_BLEND_LIGHTEN, "LIGHTEN", 0, "Lighten", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  prop = RNA_def_property(srna, "gradient_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "gradient_type");
  RNA_def_property_enum_items(prop, gradient_type_items);
  RNA_def_property_ui_text(prop, "Gradient Type", "Shape of the selection gradient");
  RNA_def_property_update(
      prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImagePaintSettings_gradient_update");

  prop = RNA_def_property(srna, "gradient_repeat", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "gradient_repeat");
  RNA_def_property_enum_items(prop, gradient_repeat_items);
  RNA_def_property_ui_text(prop, "Gradient Repeat", "Behavior outside the gradient vector");
  RNA_def_property_update(
      prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImagePaintSettings_gradient_update");

  prop = RNA_def_property(srna, "gradient_blend_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "gradient_blend_mode");
  RNA_def_property_enum_items(prop, gradient_blend_items);
  RNA_def_property_ui_text(prop, "Gradient Blend", "Blend mode for the selection gradient");
  RNA_def_property_update(
      prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImagePaintSettings_gradient_update");

  prop = RNA_def_property(srna, "gradient_opacity", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, nullptr, "gradient_opacity");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_text(prop, "Gradient Opacity", "Overall opacity of the selection gradient");
  RNA_def_property_update(
      prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImagePaintSettings_gradient_update");

  prop = RNA_def_property(srna, "use_gradient_multi_udim", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "gradient_multi_udim", 1);
  RNA_def_property_ui_text(
      prop,
      "All UDIM Tiles",
      "Paint the gradient across every UDIM tile instead of only the active one");
  RNA_def_property_update(
      prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImagePaintSettings_gradient_update");

  prop = RNA_def_property(srna, "color_ramp", PROP_POINTER, PROP_NEVER_NULL);
  RNA_def_property_struct_type(prop, "ColorRamp");
  RNA_def_property_pointer_funcs(
      prop, "rna_ImagePaintSettings_gradient_color_ramp_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Color Ramp", "Colors of the selection gradient");
  RNA_def_property_update(
      prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImagePaintSettings_gradient_update");
}

static void rna_def_particle_edit(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static const EnumPropertyItem select_mode_items[] = {
      {SCE_SELECT_PATH, "PATH", ICON_PARTICLE_PATH, "Path", "Path edit mode"},
      {SCE_SELECT_POINT, "POINT", ICON_PARTICLE_POINT, "Point", "Point select mode"},
      {SCE_SELECT_END, "TIP", ICON_PARTICLE_TIP, "Tip", "Tip select mode"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem puff_mode[] = {
      {0, "ADD", 0, "Add", "Make hairs more puffy"},
      {1, "SUB", 0, "Sub", "Make hairs less puffy"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem length_mode[] = {
      {0, "GROW", 0, "Grow", "Make hairs longer"},
      {1, "SHRINK", 0, "Shrink", "Make hairs shorter"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem edit_type_items[] = {
      {PE_TYPE_PARTICLES, "PARTICLES", 0, "Particles", ""},
      {PE_TYPE_SOFTBODY, "SOFT_BODY", 0, "Soft Body", ""},
      {PE_TYPE_CLOTH, "CLOTH", 0, "Cloth", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* edit */

  srna = RNA_def_struct(brna, "ParticleEdit", nullptr);
  RNA_def_struct_sdna(srna, "ParticleEditSettings");
  RNA_def_struct_path_func(srna, "rna_ParticleEdit_path");
  RNA_def_struct_ui_text(srna, "Particle Edit", "Properties of particle editing mode");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);

  prop = RNA_def_property(srna, "tool", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "brushtype");
  RNA_def_property_enum_items(prop, rna_enum_particle_edit_hair_brush_items);
  RNA_def_property_enum_funcs(
      prop, nullptr, "rna_ParticleEdit_tool_set", "rna_ParticleEdit_tool_itemf");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_OPERATOR_DEFAULT);
  RNA_def_property_ui_text(prop, "Tool", "");

  prop = RNA_def_property(srna, "select_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, nullptr, "selectmode");
  RNA_def_property_enum_items(prop, select_mode_items);
  RNA_def_property_ui_text(prop, "Selection Mode", "Particle select and display mode");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_ParticleEdit_update");

  prop = RNA_def_property(srna, "use_preserve_length", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", PE_KEEP_LENGTHS);
  RNA_def_property_ui_text(prop, "Keep Lengths", "Keep path lengths constant");

  prop = RNA_def_property(srna, "use_preserve_root", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", PE_LOCK_FIRST);
  RNA_def_property_ui_text(prop, "Keep Root", "Keep root keys unmodified");

  prop = RNA_def_property(srna, "use_emitter_deflect", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", PE_DEFLECT_EMITTER);
  RNA_def_property_ui_text(prop, "Deflect Emitter", "Keep paths from intersecting the emitter");

  prop = RNA_def_property(srna, "emitter_distance", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "emitterdist");
  RNA_def_property_ui_range(prop, 0.0f, 10.0f, 10, 3);
  RNA_def_property_ui_text(
      prop, "Emitter Distance", "Distance to keep particles away from the emitter");

  prop = RNA_def_property(srna, "use_fade_time", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", PE_FADE_TIME);
  RNA_def_property_ui_text(
      prop, "Fade Time", "Fade paths and keys further away from current frame");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_ParticleEdit_update");

  prop = RNA_def_property(srna, "use_auto_velocity", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", PE_AUTO_VELOCITY);
  RNA_def_property_ui_text(prop, "Auto Velocity", "Calculate point velocities automatically");

  prop = RNA_def_property(srna, "show_particles", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", PE_DRAW_PART);
  RNA_def_property_ui_text(prop, "Display Particles", "Display actual particles");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_ParticleEdit_redo");

  prop = RNA_def_property(srna, "use_default_interpolate", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", PE_INTERPOLATE_ADDED);
  RNA_def_property_ui_text(
      prop, "Interpolate", "Interpolate new particles from the existing ones");
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "default_key_count", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "totaddkey");
  RNA_def_property_range(prop, 2, SHRT_MAX);
  RNA_def_property_ui_range(prop, 2, 20, 10, 3);
  RNA_def_property_ui_text(prop, "Keys", "How many keys to make new particles with");

  prop = RNA_def_property(srna, "brush", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "ParticleBrush");
  RNA_def_property_pointer_funcs(prop, "rna_ParticleEdit_brush_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Brush", "");

  prop = RNA_def_property(srna, "display_step", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "draw_step");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_range(prop, 1, 10);
  RNA_def_property_ui_text(prop, "Steps", "How many steps to display the path with");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_ParticleEdit_redo");

  prop = RNA_def_property(srna, "fade_frames", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 1, 100);
  RNA_def_property_ui_text(prop, "Frames", "How many frames to fade");
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_ParticleEdit_update");

  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_flag(prop, PROP_CONTEXT_UPDATE);
  RNA_def_property_enum_sdna(prop, nullptr, "edittype");
  RNA_def_property_enum_items(prop, edit_type_items);
  RNA_def_property_ui_text(prop, "Type", "");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_ParticleEdit_redo");

  prop = RNA_def_property(srna, "is_editable", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_ParticleEdit_editable_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Editable", "A valid edit mode exists");

  prop = RNA_def_property(srna, "is_hair", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(prop, "rna_ParticleEdit_hair_get", nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Hair", "Editing hair");

  prop = RNA_def_property(srna, "object", PROP_POINTER, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Object", "The edited object");

  prop = RNA_def_property(srna, "shape_object", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_CONTEXT_UPDATE);
  RNA_def_property_ui_text(prop, "Shape Object", "Outer shape to use for tools");
  RNA_def_property_pointer_funcs(prop, nullptr, nullptr, nullptr, "rna_Mesh_object_poll");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_ParticleEdit_redo");

  /* brush */

  srna = RNA_def_struct(brna, "ParticleBrush", nullptr);
  RNA_def_struct_sdna(srna, "ParticleBrushData");
  RNA_def_struct_path_func(srna, "rna_ParticleBrush_path");
  RNA_def_struct_ui_text(srna, "Particle Brush", "Particle editing brush");

  prop = RNA_def_property(srna, "size", PROP_INT, PROP_PIXEL);
  RNA_def_property_range(prop, 1, SHRT_MAX);
  RNA_def_property_ui_range(prop, 1, MAX_BRUSH_PIXEL_RADIUS, 10, 3);
  RNA_def_property_ui_text(prop, "Radius", "Radius of the brush in pixels");

  prop = RNA_def_property(srna, "strength", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_range(prop, 0.001, 1.0);
  RNA_def_property_ui_text(prop, "Strength", "Brush strength");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_AMOUNT);

  prop = RNA_def_property(srna, "count", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 1, 1000);
  RNA_def_property_ui_range(prop, 1, 100, 10, 3);
  RNA_def_property_ui_text(prop, "Count", "Particle count");

  prop = RNA_def_property(srna, "steps", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, nullptr, "step");
  RNA_def_property_range(prop, 1, SHRT_MAX);
  RNA_def_property_ui_range(prop, 1, 50, 10, 3);
  RNA_def_property_ui_text(prop, "Steps", "Brush steps");

  prop = RNA_def_property(srna, "puff_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "invert");
  RNA_def_property_enum_items(prop, puff_mode);
  RNA_def_property_ui_text(prop, "Puff Mode", "");

  prop = RNA_def_property(srna, "use_puff_volume", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", PE_BRUSH_DATA_PUFF_VOLUME);
  RNA_def_property_ui_text(
      prop,
      "Puff Volume",
      "Apply puff to unselected end-points (helps maintain hair volume when puffing root)");

  prop = RNA_def_property(srna, "length_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "invert");
  RNA_def_property_enum_items(prop, length_mode);
  RNA_def_property_ui_text(prop, "Length Mode", "");

  /* dummy */
  prop = RNA_def_property(srna, "curve", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_pointer_funcs(prop, "rna_ParticleBrush_curve_get", nullptr, nullptr, nullptr);
  RNA_def_property_ui_text(prop, "Curve", "");
}

/* srna -- gpencil speed guides */
static void rna_def_gpencil_guides(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "GPencilSculptGuide", nullptr);
  RNA_def_struct_sdna(srna, "GP_Sculpt_Guide");
  RNA_def_struct_path_func(srna, "rna_GPencilSculptGuide_path");
  RNA_def_struct_ui_text(srna, "Grease Pencil Sculpt Guide", "Guides for drawing");

  static const EnumPropertyItem prop_gpencil_guidetypes[] = {
      {GP_GUIDE_CIRCULAR, "CIRCULAR", 0, "Circular", "Use single point to create rings"},
      {GP_GUIDE_RADIAL, "RADIAL", 0, "Radial", "Use single point as direction"},
      {GP_GUIDE_PARALLEL, "PARALLEL", 0, "Parallel", "Parallel lines"},
      {GP_GUIDE_GRID, "GRID", 0, "Grid", "Grid allows horizontal and vertical lines"},
      {GP_GUIDE_ISO, "ISO", 0, "Isometric", "Grid allows isometric and vertical lines"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem prop_gpencil_guide_references[] = {
      {GP_GUIDE_REF_CURSOR, "CURSOR", 0, "Cursor", "Use cursor as reference point"},
      {GP_GUIDE_REF_CUSTOM, "CUSTOM", 0, "Custom", "Use custom reference point"},
      {GP_GUIDE_REF_OBJECT, "OBJECT", 0, "Object", "Use object as reference point"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  prop = RNA_def_property(srna, "use_guide", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_guide", false);
  RNA_def_property_boolean_default(prop, false);
  RNA_def_property_ui_text(prop, "Use Guides", "Enable speed guides");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_snapping", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "use_snapping", false);
  RNA_def_property_boolean_default(prop, false);
  RNA_def_property_ui_text(
      prop, "Use Snapping", "Enable snapping to guides angle or spacing options");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "reference_object", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "reference_object");
  RNA_def_property_ui_text(prop, "Object", "Object used for reference point");
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_ID_SELF_CHECK);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImaPaint_viewport_update");

  prop = RNA_def_property(srna, "reference_point", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "reference_point");
  RNA_def_property_enum_items(prop, prop_gpencil_guide_references);
  RNA_def_property_ui_text(prop, "Type", "Type of speed guide");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImaPaint_viewport_update");

  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "type");
  RNA_def_property_enum_items(prop, prop_gpencil_guidetypes);
  RNA_def_property_ui_text(prop, "Type", "Type of speed guide");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "angle", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, nullptr, "angle");
  RNA_def_property_range(prop, -(M_PI * 2.0f), (M_PI * 2.0f));
  RNA_def_property_ui_text(prop, "Angle", "Direction of lines");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "angle_snap", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, nullptr, "angle_snap");
  RNA_def_property_range(prop, -(M_PI * 2.0f), (M_PI * 2.0f));
  RNA_def_property_ui_text(prop, "Angle Snap", "Angle snapping");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "spacing", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "spacing");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_range(prop, 0.0f, FLT_MAX, 1, 3);
  RNA_def_property_ui_text(prop, "Spacing", "Guide spacing");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "location", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_float_sdna(prop, nullptr, "location");
  RNA_def_property_array(prop, 3);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Location", "Custom reference point for guides");
  RNA_def_property_range(prop, -FLT_MAX, FLT_MAX);
  RNA_def_property_ui_range(prop, -FLT_MAX, FLT_MAX, 1, 3);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, "rna_ImaPaint_viewport_update");
}

static void rna_def_gpencil_sculpt(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  /* == Settings == */
  srna = RNA_def_struct(brna, "GPencilSculptSettings", nullptr);
  RNA_def_struct_sdna(srna, "GP_Sculpt_Settings");
  RNA_def_struct_path_func(srna, "rna_GPencilSculptSettings_path");
  RNA_def_struct_ui_text(srna,
                         "GPencil Sculpt Settings",
                         "General properties for Grease Pencil stroke sculpting tools");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);

  prop = RNA_def_property(srna, "guide", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "GPencilSculptGuide");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_ui_text(prop, "Guide", "");

  prop = RNA_def_property(srna, "use_multiframe_falloff", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_SCULPT_SETT_FLAG_FRAME_FALLOFF);
  RNA_def_property_ui_text(
      prop,
      "Use Falloff",
      "Use falloff effect when edit in multiframe mode to compute brush effect by frame");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_thickness_curve", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_SCULPT_SETT_FLAG_PRIMITIVE_CURVE);
  RNA_def_property_ui_text(prop, "Use Curve", "Use curve to define primitive stroke thickness");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_scale_thickness", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_SCULPT_SETT_FLAG_SCALE_THICKNESS);
  RNA_def_property_ui_text(
      prop, "Scale Stroke Thickness", "Scale the stroke thickness when transforming strokes");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_automasking_stroke", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_SCULPT_SETT_FLAG_AUTOMASK_STROKE);
  RNA_def_property_ui_text(prop, "Auto-Masking Strokes", "Affect only strokes below the cursor");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_automasking_layer_stroke", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_SCULPT_SETT_FLAG_AUTOMASK_LAYER_STROKE);
  RNA_def_property_ui_text(prop, "Auto-Masking Layer", "Affect only strokes below the cursor");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_automasking_material_stroke", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "flag", GP_SCULPT_SETT_FLAG_AUTOMASK_MATERIAL_STROKE);
  RNA_def_property_ui_text(prop, "Auto-Masking Material", "Affect only strokes below the cursor");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_automasking_layer_active", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", GP_SCULPT_SETT_FLAG_AUTOMASK_LAYER_ACTIVE);
  RNA_def_property_ui_text(prop, "Auto-Masking Layer", "Affect only the Active Layer");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  prop = RNA_def_property(srna, "use_automasking_material_active", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(
      prop, nullptr, "flag", GP_SCULPT_SETT_FLAG_AUTOMASK_MATERIAL_ACTIVE);
  RNA_def_property_ui_text(prop, "Auto-Masking Material", "Affect only the Active Material");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  /* custom falloff curve */
  prop = RNA_def_property(srna, "multiframe_falloff_curve", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "cur_falloff");
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_ui_text(
      prop, "Curve", "Custom curve to control falloff of brush effect by Grease Pencil frames");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  /* custom primitive curve */
  prop = RNA_def_property(srna, "thickness_primitive_curve", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, nullptr, "cur_primitive");
  RNA_def_property_struct_type(prop, "CurveMapping");
  RNA_def_property_ui_text(prop, "Curve", "Custom curve to control primitive thickness");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_SCENE | ND_TOOLSETTINGS, nullptr);

  /* lock axis */
  prop = RNA_def_property(srna, "lock_axis", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, nullptr, "lock_axis");
  RNA_def_property_enum_items(prop, rna_enum_gpencil_lock_axis_items);
  RNA_def_property_ui_text(prop, "Lock Axis", "");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_GPENCIL | ND_DATA, nullptr);

  /* threshold for cutter */
  prop = RNA_def_property(srna, "intersection_threshold", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, nullptr, "isect_threshold");
  RNA_def_property_range(prop, 0.0f, 10.0f);
  RNA_def_property_float_default(prop, 0.1f);
  RNA_def_property_ui_text(prop, "Threshold", "Threshold for stroke intersections");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
}

static void rna_def_curves_sculpt(BlenderRNA *brna)
{
  StructRNA *srna;

  srna = RNA_def_struct(brna, "CurvesSculpt", "Paint");
  RNA_def_struct_path_func(srna, "rna_CurvesSculpt_path");
  RNA_def_struct_ui_text(srna, "Curves Sculpt Paint", "");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);
}

void RNA_def_sculpt_paint(BlenderRNA *brna)
{
  /* *** Non-Animated *** */
  RNA_define_animate_sdna(false);
  rna_def_paint_curve(brna);
  rna_def_paint(brna);
  rna_def_unified_paint_settings(brna);
  rna_def_mesh_automasking_settings(brna);
  rna_def_sculpt(brna);
  rna_def_uv_sculpt(brna);
  rna_def_gp_paint(brna);
  rna_def_gp_vertexpaint(brna);
  rna_def_gp_sculptpaint(brna);
  rna_def_gp_weightpaint(brna);
  rna_def_vertex_paint(brna);
  rna_def_paint_mode(brna);
  rna_def_image_paint(brna);
  rna_def_particle_edit(brna);
  rna_def_gpencil_guides(brna);
  rna_def_gpencil_sculpt(brna);
  rna_def_curves_sculpt(brna);
  RNA_define_animate_sdna(true);
}

}  // namespace blender

#endif
