/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * General operations for brushes.
 */

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include "BLI_array.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_utildefines.h"

#include "DNA_brush_enums.h"
#include "DNA_color_types.h"
#include "DNA_object_enums.h"
#include "DNA_userdef_enums.h"

namespace blender {

enum class PaintMode : int8_t;
enum ePaintMaterialEraseMode : int8_t;
struct Brush;
struct BrushCurvePatchSlotChannel;
struct BrushCurvePatchTextureSlot;
struct BrushMaterialPaint;
struct ImBuf;
struct Image;
struct ImagePool;
struct Main;
struct MTex;
struct Paint;
struct Scene;
struct UnifiedPaintSettings;

/* Globals for brush execution. */

void BKE_brush_system_init();
void BKE_brush_system_exit();

/* Data-block functions. */

/**
 * \note Resulting brush will have two users: one as a fake user,
 * another is assumed to be used by the caller.
 */
Brush *BKE_brush_add(Main *bmain, const char *name, eObjectMode ob_mode);
/**
 * Delete a Brush.
 */
bool BKE_brush_delete(Main *bmain, Brush *brush);

/**
 * Perform deep-copy of a Brush and its 'children' data-blocks.
 *
 * \param dupflag: Controls which sub-data are also duplicated
 * (see #eDupli_ID_Flags in DNA_userdef_types.h).
 * \param duplicate_options: Additional context information about current duplicate call (e.g. if
 * it's part of a higher-level duplication or not, etc.). (see #eLibIDDuplicateFlags in
 * BKE_lib_id.hh).
 *
 * \warning By default, this functions will clear all \a bmain #ID.idnew pointers
 * (#BKE_main_id_newptr_and_tag_clear), and take care of post-duplication updates like remapping to
 * new IDs (#BKE_libblock_relink_to_newid).
 * If \a #LIB_ID_DUPLICATE_IS_SUBPROCESS duplicate option is passed on (typically when duplication
 * is called recursively from another parent duplication operation), the caller is responsible to
 * handle all of these operations.
 *
 * \note Caller MUST handle updates of the depsgraph (#DAG_relations_tag_update).
 */
Brush *BKE_brush_duplicate(Main *bmain,
                           Brush *brush,
                           eDupli_ID_Flags dupflag,
                           /*eLibIDDuplicateFlags*/ uint duplicate_options);

/**
 * Give \a brush (a freshly copied Curve Patch snapshot, or another private duplicate) its own copy
 * of every Tex data-block it still shares with the brush it was copied from: the brush and mask
 * textures, the face-set texture, the Curve Patch zone and stamp-list textures, and the PBR
 * channel sources. A brush copy only adds a user to each Tex, so without this an edit of one Tex
 * (a new image, a cleared slot) would change the source brush and every other duplicate too. A Tex
 * the brush alone uses is left as it is; the Images stay shared.
 */
void BKE_brush_textures_unshare(Main &bmain, Brush &brush);
/**
 * Add grease pencil settings.
 */
void BKE_brush_init_gpencil_settings(Brush *brush);

void BKE_brush_init_mesh_automasking_settings(Brush *brush);

/**
 * Opt a brush authored for Image Paint into Sculpt Mode (used by material paint brush sync).
 * Sets the matching #Brush.sculpt_brush_type and allocates the Sculpt-only settings. No-op when
 * the brush is already enabled for Sculpt Mode.
 */
void BKE_brush_enable_sculpt_mode_from_image_paint(Brush *brush);

void BKE_brush_init_curves_sculpt_settings(Brush *brush);

/**
 * Allocates a #BrushMaterialPaint holding the default per-channel state, owned by the caller.
 * Used both for #Brush.material_paint and for the scene-level per-brush presets, so that the
 * defaults are defined in exactly one place.
 */
BrushMaterialPaint *BKE_brush_material_paint_create_default();

/**
 * Lazily allocates #Brush.material_paint if null.
 * Call before reading/writing per-channel material paint values.
 */
void BKE_brush_material_paint_ensure(Brush *brush);

/**
 * Applies an #ePaintMaterialEraseMode as a one-shot preset to \a brush, mirroring the stock
 * texture-paint Erase brushes: distance falloff, antialiasing and pressure-driven size. Only
 * runs when the mode is picked (from the RNA update callback or while creating the brush), so
 * user tweaks to those parameters are kept in between.
 */
void BKE_brush_material_erase_mode_apply(Brush *brush, ePaintMaterialEraseMode mode);

/** True for the shared material Erase brush (Sculpt and Texture/Image paint). */
bool BKE_brush_is_material_erase(const Brush &brush);

/**
 * Adds a new local Erase brush for Sculpt Mode PBR Paint: a #Brush with
 * #SCULPT_BRUSH_TYPE_ERASE, stock Erase stroke defaults and a default channel selection.
 * Duplicate names are made unique automatically. Activation is the caller's job (typically
 * #BKE_paint_brush_set_synced). \a paint is used to sync the unified size.
 */
Brush *BKE_brush_material_erase_create_default(Main *bmain, Paint *paint);

/**
 * Returns the local Sculpt Erase brush, preferring the one named "Erase", creating it (with the
 * preset snapshot taken) when none exists. \a r_created tells the caller which happened.
 * Linked brushes are never reused. Activation is the caller's job.
 */
Brush *BKE_brush_material_erase_ensure(Main &bmain,
                                       Scene &scene,
                                       Paint &paint,
                                       bool *r_created = nullptr);

/** True for the default Mask brush created by #BKE_brush_material_mask_ensure. */
bool BKE_brush_is_material_mask_default(const Brush &brush);

/**
 * Adds a new local Mask brush: a #Brush with a hard edge, stock stroke defaults and
 * #BRUSH_MATERIAL_PAINT_MASK_DEFAULT_BRUSH set. Usable in Sculpt and Texture Paint. Activation is
 * the caller's job (typically #BKE_paint_material_mask_brush_set).
 */
Brush *BKE_brush_material_mask_create_default(Main *bmain, Paint *paint);

/**
 * Returns the default Mask brush, preferring the one named "Mask", creating it (with the preset
 * snapshot taken) when none exists. \a r_created tells the caller which happened. Linked brushes
 * are never reused. Activation is the caller's job.
 */
Brush *BKE_brush_material_mask_ensure(Main &bmain,
                                      Scene &scene,
                                      Paint &paint,
                                      bool *r_created = nullptr);

/**
 * Deep-copies channels (including each channel's source correction)/base_color/flags/
 * shared_source_mapping from \a src. The caller owns the result and must eventually pass it to
 * #BKE_brush_material_paint_free.
 *
 * \param flag: #LIB_ID_CREATE_ / #LIB_ID_COPY_ flags, as passed to the surrounding copy
 * function. Unless #LIB_ID_CREATE_NO_USER_REFCOUNT is set, a counted reference (#id_us_plus) is
 * taken on every #Tex any channel's #BrushMaterialPaintChannel.source_mtex.tex or
 * #BrushMaterialPaint.shared_source_mapping.tex points at, and on both source materials
 * (#BrushMaterialPaint.source_material and #BrushMaterialPaint.source_layer_owner).
 */
BrushMaterialPaint *BKE_brush_material_paint_copy(const BrushMaterialPaint &src, int flag);

/**
 * Frees \a material_paint, including every channel's source correction. No-op if \a material_paint
 * is null.
 *
 * \param do_user_refcount: When true, drop a counted reference (#id_us_min) on every #Tex and
 * source material the struct holds. Required when the owner is not an ID whose `foreach_id` pass
 * already walked those pointers — notably #PaintMaterialBrushPreset on remove/purge. Must stay
 * false for #brush_free_data / #scene_free (foreach_id already ran) and for copies made with
 * #LIB_ID_CREATE_NO_USER_REFCOUNT, or user counts double-decrement.
 */
void BKE_brush_material_paint_free(BrushMaterialPaint *material_paint,
                                   bool do_user_refcount = false);

/**
 * Overwrites \a dst's channels (including each channel's source correction)/base_color/flags/
 * shared_source_mapping from \a src, adjusting #Tex and source-material user counts for every
 * reference gained or dropped. \a dst == \a src (self-assignment) is a defined no-op.
 */
void BKE_brush_material_paint_copy_into(BrushMaterialPaint &dst, const BrushMaterialPaint &src);

/**
 * Clear dangling PBR Paint ID pointers that survived an undo step.
 *
 * Brushes are `no undo` IDs, so they cross undo boundaries intact. The IDs they reference
 * (channel textures, the shared source mapping texture, and both source materials) are regular
 * data-blocks that an undo step may remove. Read-time remapping
 * (#read_undo_remap_noundo_data) normally clears such pointers; this is the backstop that keeps a
 * missed one from being dereferenced later, and it never touches user counts (the ID is gone,
 * decrementing it would write into freed memory).
 *
 * Compares addresses only, so a stale pointer is never dereferenced.
 */
int BKE_brush_material_paint_stale_ids_clear(Main &bmain);

/**
 * Copy the brush's paint color into the Base Color channel, when the brush has material paint
 * settings with Sync with Brush enabled.
 *
 * \param paint: the active paint, used to resolve the unified color. May be null, in which case
 * #Brush.color is read directly — the caller then only knows about the brush, not the mode.
 *
 * \note Both sync directions share one re-entrancy guard, so a sync triggered from either side
 * cannot bounce back into the other. Keeping the guard here rather than in RNA is the whole point
 * of this pair living in blenkernel: RNA reaches the Base Color from several unrelated update
 * callbacks (brush color, unified color, the channel itself), and per-file guards would not see
 * each other.
 */
void BKE_brush_material_paint_base_color_sync_to_channel(const Paint *paint, Brush *brush);

/**
 * Copy the Base Color channel into the brush's paint color, when Sync with Brush is enabled.
 * The inverse of #BKE_brush_material_paint_base_color_sync_to_channel; shares its guard.
 *
 * \a scene, when non-null and Material brush sync is on, also writes the color onto the paired
 * paint mode's #UnifiedPaintSettings. Image Editor Color reads that struct, not the shared
 * #BrushMaterialPaint, so omitting it left the Image Editor brush color stale.
 */
void BKE_brush_material_paint_base_color_sync_to_brush(Paint *paint,
                                                       Brush *brush,
                                                       Scene *scene = nullptr);

/**
 * Tag a linked brush as having changed settings so an indicator can be displayed to the user,
 * showing that the brush settings differ from the state of the imported brush asset. Call
 * every time a user visible change to the brush is done.
 *
 * Since this is meant to indicate brushes that are known to differ from the linked source file,
 * tagging is only performed for linked brushes. File local brushes are normal data-blocks that get
 * saved with the file, and don't need special attention by the user.
 *
 * For convenience, null may be passed for \a brush.
 */
void BKE_brush_tag_unsaved_changes(Brush *brush);

/**
 * Recompute #Brush.drag_kind from #Brush.sculpt_brush_type / #Brush.stroke_method /
 * #Brush.cloth_deform_type. Call whenever any of those three fields changes (RNA setters for all
 * three call this; see `rna_brush.cc`) so #Brush.drag_kind never goes stale relative to them.
 */
void BKE_brush_drag_kind_update(Brush *brush);
/* Curve Patch texture slots.
 *
 * The list owns a user on each slot's texture (registered in `brush_foreach_id`), which is why
 * adding and removing a slot cannot be open-coded by every caller. Both the operators and the RNA
 * collection go through here so the two can never drift apart. Neither tags the brush or sends a
 * notifier -- that belongs to the caller, which knows its own context. */

/** Append a slot with its DNA defaults and make it the active one. */
BrushCurvePatchTextureSlot *BKE_brush_curve_patch_texture_slot_add(Brush &brush);

/**
 * Drop a slot, releasing the user it holds on its texture. Adjusts
 * #BrushCurvePatchSettings.texture_active_index so it still names a remaining slot: unchanged when
 * a later slot is removed, decremented when an earlier one is, clamped when the active slot was
 * last, and 0 when the list is empty.
 *
 * \return false when `slot` is not in this brush's list, in which case nothing is changed.
 */
bool BKE_brush_curve_patch_texture_slot_remove(Brush &brush, BrushCurvePatchTextureSlot &slot);

/* PBR slots.
 *
 * A slot of kind #BRUSH_CURVE_PATCH_SLOT_PBR declares which material paint channels it covers
 * (#BrushCurvePatchTextureSlot::channels, one entry per #eMaterialPaintChannel) and carries one
 * #Image per channel. The list owns a user on every non-null channel image, exactly as it does on
 * a TEXTURE slot's texture, so assigning and clearing an image cannot be open-coded by every
 * caller either. */

/** The slot's image for \a channel (#eMaterialPaintChannel), or null. The answer describes the
 * DATA, independent of the slot kind: a slot flipped away from PBR keeps its images, so a caller
 * that reads them (Python, a UI row) still sees what is there. Stamps and counts filter by kind
 * where they consume the image. */
Image *BKE_brush_curve_patch_slot_image_get(const BrushCurvePatchTextureSlot &slot, int channel);

/**
 * Assign \a image to \a channel (#eMaterialPaintChannel), keeping the list's user bookkeeping:
 * takes a user on the new image, drops the one held on the previous. The slot's kind is left
 * alone -- setting an image does not flip an unrelated slot to PBR, and a PBR slot may pre-fill
 * its channels before (or after) the user switches the kind. Null clears the channel.
 */
void BKE_brush_curve_patch_slot_image_set(BrushCurvePatchTextureSlot &slot,
                                          int channel,
                                          Image *image);

/** Whether the slot DECLARES \a channel (its #BrushCurvePatchSlotChannel::enabled is set).
 * Declaring is intent, not data: an enabled channel without an image is skipped by the stamps. */
bool BKE_brush_curve_patch_slot_channel_enabled(const BrushCurvePatchTextureSlot &slot,
                                                int channel);

/** Enable / disable \a channel in the slot's declared set. */
void BKE_brush_curve_patch_slot_channel_enable(BrushCurvePatchTextureSlot &slot,
                                               int channel,
                                               bool enable);

/** How many enabled channels of a PBR slot actually hold an image (for UI row labels). */
int BKE_brush_curve_patch_slot_image_count(const BrushCurvePatchTextureSlot &slot);

/** Whether \a channel takes part in a stamp: declared AND carrying an image. The one predicate
 * stamps, counts, names and digests share, so they cannot disagree on what a half-filled set
 * contributes. */
bool BKE_brush_curve_patch_slot_channel_active(const BrushCurvePatchTextureSlot &slot, int channel);

/** The channel (#eMaterialPaintChannel) a dropped image should fill: the first enabled channel
 * without an image, otherwise Base Color. */
int BKE_brush_curve_patch_slot_drop_channel(const BrushCurvePatchTextureSlot &slot);

/**
 * The slot that owns \a channel, or null when the channel is not one of this brush's slot rows.
 * A channel is only addressable through its owner, and every caller (RNA collection accessors, the
 * drop-target lookup) needs both the slot and the channel's index inside it, so the lookup and the
 * arithmetic live here once.
 *
 * \param r_channel_index: Optionally returns the index inside the slot (#eMaterialPaintChannel).
 */
BrushCurvePatchTextureSlot *BKE_brush_curve_patch_slot_from_channel(
    Brush &brush, const BrushCurvePatchSlotChannel &channel, int *r_channel_index = nullptr);
const BrushCurvePatchTextureSlot *BKE_brush_curve_patch_slot_from_channel(
    const Brush &brush, const BrushCurvePatchSlotChannel &channel, int *r_channel_index = nullptr);

/** Index of the slot owning \a channel in the brush's texture list, or -1. */
int BKE_brush_curve_patch_slot_index_from_channel(const Brush &brush,
                                                  const BrushCurvePatchSlotChannel &channel);

/**
 * The material paint channels (bit per #eMaterialPaintChannel) the brush's Curve Patch texture list
 * supplies stamps for: the enabled, filled channels of its PBR slots and every channel a Material
 * slot's material can supply. Zero unless the brush stamps its list (STAMPS + Texture List). A
 * stroke paints these channels as well as the ones the brush switches on itself, so a slot's set
 * does not need the same channels enabled a second time on the brush.
 */
uint32_t BKE_brush_curve_patch_slot_channel_mask(const Brush &brush);

/** Resolution of a MATERIAL slot's bake: the slot's own size, or the default when it is unset. */
int BKE_brush_curve_patch_slot_bake_size(const BrushCurvePatchTextureSlot &slot);

/** The name a slot shows in the texture list: the first enabled channel image's name for a PBR
 * set ("PBR Set" while it is empty), the texture's name for a TEXTURE slot, empty when unset. */
StringRefNull BKE_brush_curve_patch_slot_display_name(const BrushCurvePatchTextureSlot &slot);

float2 BKE_brush_jitter_pos(const Paint &paint, const Brush &brush, const float2 &pos);
void BKE_brush_randomize_texture_coords(Paint *paint, bool mask);

/* Brush curve. */

/**
 * Library Operations
 */
void BKE_brush_curve_preset(Brush *b, eCurveMappingPreset preset);

namespace bke::brush {
void common_pressure_curves_init(Brush &brush);
}

/**
 * Combine the brush strength based on the distances and brush settings with the existing factors.
 */
void BKE_brush_calc_curve_factors(eBrushCurvePreset preset,
                                  const CurveMapping *cumap,
                                  Span<float> distances,
                                  float brush_radius,
                                  MutableSpan<float> factors);
/**
 * Uses the brush curve control to find a strength value between 0 and 1.
 */
float BKE_brush_curve_strength_clamped(const Brush *br, float p, float len);
/**
 * Uses the brush curve control to find a strength value.
 */
float BKE_brush_curve_strength(eBrushCurvePreset preset,
                               const CurveMapping *cumap,
                               float distance,
                               float brush_radius);
float BKE_brush_curve_strength(const Brush *br, float p, float len);

/**
 * Which brush texture slot an #MTex represents to the sampling helpers below. The slot identity
 * is mode-dependent (#BKE_brush_mask_texture_get hands out #Brush.mtex in Sculpt), so it cannot
 * be derived from the #MTex pointer alone: every caller that samples a brush texture passes the
 * slot it resolved, and the flip/invert contract keys off the slot, not off a boolean or off the
 * caller's intent.
 */
enum class eBrushTextureSlot {
  /** The brush texture slot: #Brush.mtex everywhere but Sculpt, where the legacy mask slot
   * (#BKE_brush_mask_texture_get with #OB_MODE_SCULPT) hands out #Brush.mtex too. This is the
   * only slot the brush's #BRUSH_TEXTURE_INVERT applies to. */
  Brush,
  /** The mask texture slot: #Brush.mask_mtex outside Sculpt (also sampled as the PBR Mask
   * Source). Its own invert is #BRUSH_MASK_TEXTURE_INVERT applied by
   * #BKE_brush_mask_tex_coverage_finish — never the brush texture's invert here. */
  Mask,
  /** The Face Set color texture (#Brush.face_set_color_mtex). */
  FaceSetColor,
  /** A PBR channel source (#BrushMaterialPaint). Its flip comes from the shared source mapping
   * (#BrushMaterialPaint::shared_source_mapping): static X/Y bits plus the stroke's random draw,
   * the same contract as the brush texture. The brush's own #BRUSH_TEXTURE_INVERT never applies
   * here; a channel's mirroring is a source-level setting, not an #MTex flag on the brush slot. */
  ChannelSource,
};

/* Sampling. */

/**
 * Generic texture sampler for 3D painting systems.
 * point has to be either in region space mouse coordinates,
 * or 3d world coordinates for 3D mapping.
 *
 * RGBA outputs straight alpha.
 *
 * The aspect ratio correction is read from #bke::PaintRuntime, see
 * #BKE_brush_tex_aspect_correction_update.
 *
 * \a slot states what \a mtex is: it decides the flip contract (static bits plus, for the Brush
 * and Mask slots, the stroke's random flip) and whether the brush texture's invert applies, and
 * cannot be overridden per call — see #eBrushTextureSlot. The invert runs before
 * #Brush::texture_sample_bias, the order every sampler in this module keeps.
 */
float BKE_brush_sample_tex_3d(const Paint *paint,
                              const Brush *br,
                              const MTex *mtex,
                              eBrushTextureSlot slot,
                              const float3 &point,
                              float4 &rgba,
                              int thread,
                              ImagePool *pool,
                              bool apply_texture_clip = true);
/**
 * Mask Texture coverage at \a point for the legacy (non-Area) mapping modes.
 *
 * The result includes the per-dab Mask Influence Random
 * (#bke::PaintRuntime::mask_influence_random_value), so it depends on stroke state: outside a
 * stroke, and between dabs, the runtime value is 1.0 and the coverage is the configured one.
 * Callers that preview the mask while a stroke is running must not use this function (see
 * #BKE_brush_mask_tex_area_coverage's \a apply_influence_random).
 */
float BKE_brush_sample_masktex(
    const Paint *paint, Brush *br, const float2 &point, int thread, ImagePool *pool);

/**
 * Whether the \a slot's texture mirrors its sampled coordinates on X/Y: the slot's static flip
 * bits (#MTEX_MAPPING_FLIP_X / #MTEX_MAPPING_FLIP_Y) XOR-ed with this dab's random flip
 * (#MTEX_MAPPING_FLIP_X_RANDOM / #MTEX_MAPPING_FLIP_Y_RANDOM, drawn by the stroke step into
 * #bke::PaintRuntime::tex_flip — XOR is the correct static-plus-random rule: with the random draw
 * always at 50/50 it exactly mirrors "the slot flips unless the random draw undoes it").
 *
 * The Brush and Mask slots read their own random state. The PBR channel slot reads the channel
 * fields (#bke::BrushTexFlipState::channel_x/_y) and its static bits, which the caller copies from
 * #BrushMaterialPaint::shared_source_mapping into the sampled #MTex; it never takes the brush
 * texture's invert. The Face Set color slot reads its static bits only. The random part is stroke
 * state: outside a stroke it reads as off, so previews show the static flip only. \a paint may be
 * null (a caller without a paint context), in which case only the static bits are consulted.
 *
 * A slot in #MTEX_MAP_MODE_3D always reads as unflipped: object-space sampling has no 2D
 * coordinate to mirror. Both the stroke samplers and the previews get that rule from here.
 */
bool BKE_brush_mtex_flip_x(const Paint *paint, const MTex &mtex, eBrushTextureSlot slot);
bool BKE_brush_mtex_flip_y(const Paint *paint, const MTex &mtex, eBrushTextureSlot slot);

/**
 * Whether random flips (#MTEX_MAPPING_FLIP_X_RANDOM / #MTEX_MAPPING_FLIP_Y_RANDOM) can change
 * what sampling \a mtex produces: it must reference a texture, carry at least one random flip
 * bit, and not use object-space (#MTEX_MAP_MODE_3D) mapping, which takes no flip at all (see
 * #BKE_brush_mtex_flip_x). The stroke step uses this to decide whether a random flip draw is
 * worth an RNG call, so "no texture" and "3D mapping" must not spend one.
 */
bool BKE_brush_mtex_flip_random_eligible(const MTex &mtex);

/**
 * The same eligibility test for the PBR channel sources, which share one mapping instead of
 * carrying a texture per slot: \a material_paint must be non-null, its
 * #BrushMaterialPaint::shared_source_mapping must carry a random flip bit and must not be in
 * #MTEX_MAP_MODE_3D. Shared with #BKE_brush_mtex_flip_random_eligible's callers so the stroke and
 * the samplers cannot disagree on whether a flip draw applies.
 */
bool BKE_brush_material_source_flip_random_eligible(const BrushMaterialPaint *material_paint);

/**
 * The raw value flip behind #BKE_brush_texture_invert_apply, with no #Brush / slot gating: the
 * scalar intensity flips in place, and an RGB(A) sample additionally flips its alpha, so both
 * consumers (the scalar masks and the 2D stamp coverage) see the same flip. Only
 * #BRUSH_TEXTURE_INVERT callers should reach it, through #BKE_brush_texture_invert_apply.
 */
void BKE_brush_texture_invert_value(bool has_rgb, float *r_intensity, float r_rgba[4]);

/**
 * The one invert contract for brush texture sampling, in one place: applies
 * #BKE_brush_texture_invert_value only when \a slot is #eBrushTextureSlot::Brush and the brush
 * has #BRUSH_TEXTURE_INVERT set — a Mask, Face Set color or PBR channel source sample never
 * takes the brush texture's invert, and a caller cannot apply or skip it selectively.
 *
 * Contract order: the invert is part of the raw sample — it runs right after the texture
 * evaluation (and the image fallback where a caller has one, so the substituted value is covered
 * too), before #Brush::texture_sample_bias and before any colorspace decode. Callers that apply
 * a bias do so after this call; callers that decode colorspace do so after the bias. Out-of-bounds
 * clip paths return zero without reaching the invert, so the stamp keeps its footprint.
 */
void BKE_brush_texture_invert_apply(eBrushTextureSlot slot,
                                    const Brush &brush,
                                    bool has_rgb,
                                    float *r_intensity,
                                    float r_rgba[4]);

/**
 * Applies the RAMP/CUTOFF #Brush.mask_pressure curve to a Mask Texture sample. Shared by
 * #BKE_brush_sample_masktex and the PBR stroke paths that sample #Brush.mask_mtex with
 * surface-mapped coordinates. The slot's own invert is applied afterward by
 * #BKE_brush_mask_tex_coverage_finish.
 */
float BKE_brush_mask_tex_pressure_apply(const Brush &brush, const Paint &paint, float value);

/**
 * Weaken the finished Mask coverage by this dab's random factor
 * (#Brush.mask_texture_strength_random through #bke::PaintRuntime::mask_influence_random_value):
 * the result blends from 1 (no clipping) to \a coverage. Applied after the slot's invert, so it
 * scales the mask the stroke actually uses. Returns \a coverage unchanged without a paint runtime.
 *
 * This reads per-dab stroke state (#bke::PaintRuntime::mask_influence_random_value), which is 1.0
 * between dabs and outside a stroke. Any caller outside a stroke therefore sees the unweakened
 * coverage; a caller that runs during a stroke but must preview the configured mask (the cursor)
 * passes \a apply_influence_random = false where available instead of relying on that.
 */
float BKE_brush_mask_tex_influence_apply(const Paint *paint, float coverage);

/**
 * The one clamp-then-mirror rule for Mask Texture coverage: clamped to 0..1, then inverted
 * (`1 - c`) when \a invert is set. Shared by the Mask slot (#Brush.mask_mtex) and the Mask Source
 * coverage (#material::mask_coverage_finish), so neither path reimplements the formula.
 */
inline float BKE_brush_mask_coverage_finish(const bool invert, const float raw)
{
  const float clamped = math::clamp(raw, 0.0f, 1.0f);
  return invert ? 1.0f - clamped : clamped;
}

/**
 * Whether the Mask slot (#Brush.mask_mtex) mirrors its coverage: #Brush.use_mask_texture_invert,
 * stored in #Brush.flag2 as #BRUSH_MASK_TEXTURE_INVERT. Independent of the Mask Source's invert.
 * Defined in brush.cc so this header does not need the full #Brush definition.
 */
bool BKE_brush_mask_texture_invert_get(const Brush &brush);

/** #BKE_brush_mask_coverage_finish applied with the Mask slot's own invert. */
float BKE_brush_mask_tex_coverage_finish(const Brush &brush, float raw);

namespace bke {

/**
 * Stroke-lifetime cache of the Mask image's values in mask-texture space (after size, offset and
 * aspect, before the pressure curve), so a dab reads a table instead of evaluating the texture per
 * texel.
 *
 * The table is rotation independent: rake/random angles rotate the query, not the table. It is
 * filled lazily in tiles, by whichever thread first needs a tile; a thread that finds a tile not
 * ready simply evaluates the texture directly for that sample, so nothing ever blocks on a bake.
 * Only image textures without a node tree whose table fits the size cap are cached; every other
 * Mask keeps the direct path. One instance serves one stroke: it assumes the Mask settings do not
 * change while it lives.
 */
class BrushMaskLut {
 public:
  /**
   * Returns the table value at the texture coordinates \a x, \a y (the ones the texture would be
   * evaluated at) in \a r_value, or false when the caller has to evaluate the texture itself.
   * Thread safe.
   */
  bool sample(const Brush &brush,
              const float2 &aspect_correction,
              float x,
              float y,
              int thread,
              ImagePool *pool,
              float &r_value) const;

 private:
  static constexpr int tile_size_ = 64;

  void prepare(const Brush &brush, const float2 &aspect_correction) const;
  void bake_tile(const Brush &brush, int tile_x, int tile_y, int thread, ImagePool *pool) const;
  bool tile_ready(const Brush &brush, int cell_x, int cell_y, int thread, ImagePool *pool) const;

  mutable std::mutex mutex_;
  mutable std::atomic<bool> prepared_{false};
  /* The members below are written once under `mutex_` before `prepared_` is set. */
  mutable bool usable_ = false;
  mutable float2 aspect_ = float2(1.0f);
  /** Half size of the covered texture-space rectangle. */
  mutable float2 half_extent_ = float2(0.0f);
  mutable int2 cells_ = int2(0);
  mutable int2 tiles_ = int2(0);
  mutable Array<uint16_t> values_;
  /** 0: empty, 1: being baked, 2: ready. */
  mutable std::unique_ptr<std::atomic<uint8_t>[]> tile_state_;
};

}  // namespace bke

/**
 * Mask Texture coverage in the Area Plane frame: \a local is brush-local, [-1, 1] across the
 * footprint. The single implementation behind the PBR strokes, the paint cursor and the radial
 * control preview, so what is previewed is exactly what the stroke skips.
 *
 * \param lut: Optional stroke-lifetime cache, see #bke::BrushMaskLut; null evaluates directly.
 * \param paint: Provides the pressure curve; null when no #Paint is available (radial control),
 * which skips the curve.
 * \param rake_rotation: The slot's rake/random angle the stroke folded into
 * #bke::PaintRuntime::brush_rotation_sec; 0 when there is no stroke.
 * \param aspect_correction: See #BKE_brush_get_aspect_correction for #Brush.mask_mtex.
 * \param apply_influence_random: When true, applies this dab's
 * #bke::PaintRuntime::mask_influence_random_value (see #BKE_brush_mask_tex_influence_apply).
 * Overlay previews pass false: the random factor is per dab and must not make the cursor show a
 * different mask than the one the user configured.
 *
 * The Mask slot's flip contract (static bits plus the stroke's random flip, see
 * #BKE_brush_mtex_flip_x) is applied to the query coordinates before the LUT or texture
 * evaluation; the LUT caches values in mask-texture space, so the flip is a transform of the
 * request, not of the cache, and the same table serves flipped and unflipped queries.
 * \return 1 when no Mask texture is assigned.
 */
float BKE_brush_mask_tex_area_coverage(const Brush &brush,
                                       const Paint *paint,
                                       const float2 &local,
                                       float rake_rotation,
                                       const float2 &aspect_correction,
                                       int thread,
                                       ImagePool *pool,
                                       const bke::BrushMaskLut *lut = nullptr,
                                       bool apply_influence_random = true);

/**
 * Scale to apply to the texture sampling coordinates so that a non-square image texture keeps its
 * aspect ratio instead of being stretched to fill the square brush footprint.
 *
 * \return `1` on both axes unless `mtex` has #MTEX_MAPPING_PRESERVE_ASPECT enabled, uses a mapping
 * mode with a square footprint and refers to a non-square image.
 */
float2 BKE_brush_get_aspect_correction(const MTex *mtex, ImagePool *pool);

/**
 * Cache the aspect ratio correction of both brush textures in #bke::PaintRuntime, so that
 * #BKE_brush_sample_tex_3d and #BKE_brush_sample_masktex do not have to acquire the image buffer
 * for every sample. Call this whenever texture sampling is about to start, typically when a stroke
 * begins.
 */
void BKE_brush_tex_aspect_correction_update(const Paint &paint, const Brush &brush);

/**
 * Get the mask texture for this given object mode.
 *
 * This is preferred above using mtex/mask_mtex attributes directly as due to legacy these
 * attributes got switched in sculpt mode.
 */
const MTex *BKE_brush_mask_texture_get(const Brush *brush, eObjectMode object_mode);

/**
 * Get the color texture for this given object mode.
 *
 * This is preferred above using mtex/mask_mtex attributes directly as due to legacy these
 * attributes got switched in sculpt mode.
 */
const MTex *BKE_brush_color_texture_get(const Brush *brush, eObjectMode object_mode);

const MTex *BKE_brush_face_set_color_texture_get(const Brush *brush, eObjectMode object_mode);

/**
 * Copy the mask texture's stroke mapping (map mode, offset, scale, rotation, angle mode) onto
 * #Brush.face_set_color_mtex. The Face Set color map is sampled with the same screen-space
 * anchoring as the mask (the same stencil position and stroke mouse reference), so it has to
 * follow the mask's mapping rather than the main texture slot's.
 */
void BKE_brush_face_set_color_mtex_sync_mapping_from_mask(Brush &brush);

/**
 * Switch the brush's Face Sets-from-texture feature to \a mode (one of
 * #BRUSH_TEXTURE_DATA_MODE_FACE_SETS_FROM_TEXTURE or
 * #BRUSH_TEXTURE_DATA_MODE_FACE_SETS_COLOR_FROM_TEXTURE): sets #Brush::face_sets_texture_mode and
 * the effective #Brush::texture_data_mode, writes the RGB output channel, and syncs the color map
 * mapping from the mask. RNA setters and the open-assign operators share this one entry point.
 */
void BKE_brush_face_sets_texture_mode_set(Brush &brush, eBrushTextureDataMode mode);

/**
 * Radial control.
 *
 * \param r_is_color: When non-null, set true if the preview stores RGB (Material Paint source)
 * rather than a single intensity channel. The F-key overlay uses this to skip the brush-color
 * tint so a Base Color map is visible.
 * \param material_source_ibuf: Pixels to preview when the brush is in Source Mode: Material, where
 * no channel has a #Tex for #RE_texture_evaluate to sample. Passed in rather than resolved here
 * because the bake it comes from lives in the editors, which blenkernel must not reach into.
 * \param material_source_mtex: Placement for \a material_source_ibuf; required alongside it.
 */
ImBuf *BKE_brush_gen_radial_control_imbuf(Brush *br,
                                          bool secondary,
                                          bool display_gradient,
                                          bool *r_is_color = nullptr,
                                          const ImBuf *material_source_ibuf = nullptr,
                                          const MTex *material_source_mtex = nullptr);

/* Unified strength size and color. */

struct BrushColorJitterSettings {
  int flag;
  /** Jitter amounts */
  float hue;
  float saturation;
  float value;

  /** Jitter pressure curves. */
  CurveMapping *curve_hue_jitter;
  CurveMapping *curve_sat_jitter;
  CurveMapping *curve_val_jitter;
};

float3 BKE_brush_color_get(const Paint *paint, const Brush *brush);
std::optional<BrushColorJitterSettings> BKE_brush_color_jitter_get_settings(const Paint *paint,
                                                                            const Brush *brush);
float3 BKE_brush_secondary_color_get(const Paint *paint, const Brush *brush);
void BKE_brush_color_set(Paint *paint, Brush *brush, const float3 &color);

void BKE_brush_color_sync_legacy(Brush *brush);
void BKE_brush_color_sync_legacy(UnifiedPaintSettings *ups);

int BKE_brush_size_get(const Paint *paint, const Brush *brush);
void BKE_brush_size_set(Paint *paint, Brush *brush, int size);
float BKE_brush_radius_get(const Paint *paint, const Brush *brush);

float BKE_brush_unprojected_size_get(const Paint *paint, const Brush *brush);
void BKE_brush_unprojected_size_set(Paint *paint, Brush *brush, float unprojected_size);
float BKE_brush_unprojected_radius_get(const Paint *paint, const Brush *brush);

float BKE_brush_alpha_get(const Paint *paint, const Brush *brush);
void BKE_brush_alpha_set(Paint *paint, Brush *brush, float alpha);
float BKE_brush_weight_get(const Paint *paint, const Brush *brush);
void BKE_brush_weight_set(Paint *paint, Brush *brush, float value);

int BKE_brush_input_samples_get(const Paint *paint, const Brush *brush);
void BKE_brush_input_samples_set(Paint *paint, Brush *brush, int value);

bool BKE_brush_use_locked_size(const Paint *paint, const Brush *brush);
bool BKE_brush_use_alpha_pressure(const Brush *brush);
bool BKE_brush_use_size_pressure(const Brush *brush);

/**
 * Scale unprojected radius to reflect a change in the brush's 2D size.
 */
void BKE_brush_scale_unprojected_size(float *unprojected_size,
                                      int new_brush_size,
                                      int old_brush_size);

/**
 * Scale brush size to reflect a change in the brush's unprojected radius.
 */
void BKE_brush_scale_size(int *r_brush_size,
                          float new_unprojected_size,
                          float old_unprojected_size);

/* Returns true if a brush requires a cube
 * (often presented to the user as a square) tip inside a specific paint mode.
 */
bool BKE_brush_has_cube_tip(const Brush *brush, PaintMode paint_mode);

namespace bke::brush {
float normal_weight_get(const Brush &brush, bool invert);

/**
 * Name of the asset metadata property holding #Brush.sculpt_brush_type (see
 * `brush_asset_metadata_ensure`), for code classifying brush assets without loading them.
 */
inline constexpr const char *sculpt_brush_type_asset_key = "sculpt_brush_type";

/**
 * Whether a sculpt brush type is drawn in the "Paint" group of the Sculpt Mode toolbar (the color
 * brushes plus Texture Fill and Clone), as opposed to the "Sculpt" group. A toolbar-level
 * classification: the asset shelf filtering uses it, so it must follow `_tools_paint_display` in
 * `space_toolsystem_toolbar.py`.
 */
inline bool sculpt_brush_type_in_paint_group(const int sculpt_brush_type)
{
  return ELEM(sculpt_brush_type,
              SCULPT_BRUSH_TYPE_PAINT,
              SCULPT_BRUSH_TYPE_SMEAR,
              SCULPT_BRUSH_TYPE_BLUR,
              SCULPT_BRUSH_TYPE_TEXTURE_FILL,
              SCULPT_BRUSH_TYPE_ERASE,
              SCULPT_BRUSH_TYPE_CLONE);
}
}

/* debugging only */
void BKE_brush_debug_print_state(Brush *br);

/* -------------------------------------------------------------------- */
/** \name Brush Capabilities
 * Common boolean checks used during both brush evaluation and in UI drawing
 * via BrushCapabilities inside rna_brush.cc.
 * \{ */

namespace bke::brush {
bool supports_dyntopo(const Brush &brush);
bool supports_accumulate(const Brush &brush);
bool supports_topology_rake(const Brush &brush);
bool supports_auto_smooth(const Brush &brush);
bool supports_normal_radius(const Brush &brush);
bool supports_hardness(const Brush &brush);
bool supports_height(const Brush &brush);
bool supports_plane_height(const Brush &brush);
bool supports_plane_depth(const Brush &brush);
bool supports_jitter(const Brush &brush);
bool supports_normal_weight(const Brush &brush);
bool supports_rake_factor(const Brush &brush);
bool supports_persistence(const Brush &brush);
bool supports_pinch_factor(const Brush &brush);
bool supports_plane_offset(const Brush &brush);
bool supports_random_texture_angle(const Brush &brush);
bool supports_sculpt_plane(const Brush &brush);
bool supports_color(const Brush &brush);
/**
 * Whether the Curve Patch stroke method is meaningful for this brush.
 *
 * Curve Patch never reaches `do_brush_action()` — it applies its own texture-driven relief
 * directly — so the brush type only supplies strength, radius, texture and falloff. This
 * allowlist therefore names the brushes whose *result* Curve Patch can stand in for, not
 * brushes whose implementation it uses.
 */
bool supports_curve_patch(const Brush &brush);
bool supports_secondary_cursor_color(const Brush &brush);
/**
 * Whether the Face Sets From Texture popover is shown for this brush. Single source of truth for
 * both #BrushCapabilitiesSculpt.has_face_set_texture (RNA, used by the Python UI) and the brush
 * group override "record" mechanism (see #BKE_paint_brush_group_reset_from_asset).
 */
bool supports_face_set_texture(const Brush &brush);
bool supports_smooth_stroke(const Brush &brush);
bool supports_space_attenuation(const Brush &brush);
bool supports_strength_pressure(const Brush &brush);
bool supports_size_pressure(const Brush &brush);
bool supports_auto_smooth_pressure(const Brush &brush);
bool supports_hardness_pressure(const Brush &brush);
bool supports_inverted_direction(const Brush &brush);
bool supports_gravity(const Brush &brush);
bool supports_tilt(const Brush &brush);
}  // namespace bke::brush

/** \} */

}  // namespace blender
