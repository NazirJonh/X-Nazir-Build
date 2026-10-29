/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The paint layers of a *layered* material, read from and written to the DNA description.
 *
 * `Material::paint_layers` is the truth here; the node tree is generated from it, and every verb
 * goes through the `BKE_paint_layers_*` API by marker. The source is deliberately free of the old
 * graph-as-truth vocabulary -- no bindings, no graph corrections, no node identities -- so the
 * description can be edited with or without an Outliner in the context.
 */

#include <algorithm>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>

#include <fmt/format.h>

#include "AS_asset_representation.hh"

#include "DNA_ID.h"
#include "DNA_asset_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_uuid_types.h"

#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_gradient_surface.hh"
#include "BKE_paint_layers_object_mask.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_material_resolve.hh"
#include "BKE_paint_material_sync.hh"
#include "BKE_report.hh"

#include "BLI_assert.h"
#include "BLI_fileops.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_color.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "DEG_depsgraph_query.hh"

#include "IMB_imbuf_types.hh"

#include "ED_asset_image_utils.hh"
#include "ED_asset_import.hh"
#include "ED_curve_patch.hh"
#include "ED_material_bake.hh"
#include "ED_outliner_stack.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_types.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../../sculpt_paint/paint_curve_patch_layer.hh"

#include "../outliner_intern.hh"
#include "outliner_stack_layers_intern.hh"
#include "outliner_stack_source.hh"
#include "outliner_stack_source_paint_layers_intern.hh"

namespace blender::ed::outliner {

/**
 * Material Symbols codepoints the paint stack draws its icons from.
 *
 * The codepoints come from `scripts/startup/bl_ui/glyph_library/data/material_symbols.json`; the
 * font that renders them ships with Blender, so a codepoint is stable for the bundled library
 * version. Each icon keeps its built-in #ICON_* as the fallback for headless builds and as the
 * name a user finds in the rest of Blender.
 *
 * #BRUSH, #FILL and #GRADIENT are the three `space_outliner.py` repeats for its header buttons.
 * `outliner_stack_source_paint_layers_test.cc` pins their values, so a change here fails the test
 * and sends the reader to the Python comment that names the test.
 */
namespace paint_glyph {
const uint32_t BRUSH = 0xE3AE;          /* brush */
const uint32_t FILL = 0xE997;           /* colors */
const uint32_t GRADIENT = 0xE3E9;       /* gradient */
constexpr uint32_t TEXTURE = 0xE421;    /* texture */
constexpr uint32_t INVERT = 0xE891;     /* invert_colors */
constexpr uint32_t LEVELS = 0xE429;     /* tune */
/* Hue/Saturation/Value's own lens, not `colors`: that one is the Fill layer's. */
constexpr uint32_t HSL = 0xE40A;        /* color_lens */
constexpr uint32_t ANCHOR = 0xF1CD;     /* anchor */
constexpr uint32_t CELL_TOWER = 0xEBBA; /* cell_tower: the Anchor Point reader */
constexpr uint32_t LINE_CURVE = 0xF757; /* line_curve */
constexpr uint32_t COLORIZE = 0xE3B8;   /* colorize */
constexpr uint32_t SELECT_ALL = 0xE162; /* select_all */
}  // namespace paint_glyph

/* -------------------------------------------------------------------- */
/** \name Description helpers
 * \{ */

namespace {

/** The owner of a layered paint stack is always a material. */
Material &layers_owner(ID &owner)
{
  BLI_assert(GS(owner.name) == ID_MA);
  return id_cast<Material &>(owner);
}

const Material &layers_owner(const ID &owner)
{
  BLI_assert(GS(owner.name) == ID_MA);
  return id_cast<const Material &>(owner);
}

/**
 * The channel the Stack Layers header selected, from the scene's paint settings; clamped.
 *
 * The cursor is a scene setting while the channel set belongs to the material, so it can name a
 * channel \a material leaves out (Base Color by default, or after switching to another material).
 * The columns would then edit a channel that takes part nowhere, and the header menu, which only
 * offers the set, would still list it. The cursor is moved into the set here, the one place every
 * view of the stack passes through, so the header and the columns always agree.
 */
int paint_stack_selected_channel(const StackReadContext &ctx, const Material &material)
{
  if (ctx.scene == nullptr || ctx.scene->toolsettings == nullptr) {
    return PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  }
  int &cursor = ctx.scene->toolsettings->paint_mode.stack_layer_channel;
  const int clamped = BKE_paint_layers_channel_clamp_to_set(
      material,
      (cursor >= 0 && cursor < PAINT_MATERIAL_CHANNEL_NUM) ? cursor :
                                                              PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  cursor = clamped;
  return clamped;
}

/** Leave Mask mode when the layer being edited in it is the one \a layer is: its mask is going
 * away or off, and a stroke into nothing must not be what the mode means. */
void layers_mask_edit_end_if_active(bContext &C,
                                    const Material &material,
                                    const MaterialPaintLayer &layer)
{
  const Scene *scene = CTX_data_scene(&C);
  if (scene == nullptr || scene->toolsettings == nullptr ||
      scene->toolsettings->paint_mode.layer_target_mode != PAINT_LAYER_TARGET_MASK)
  {
    return;
  }
  /* The row, or a folder it holds the edited row in: removing the folder takes the mask along. */
  if (BKE_paint_layers_subtree_contains(layer, material.active_layer_marker)) {
    sculpt_paint::paint_stack_target_mode_set(C, PAINT_LAYER_TARGET_CONTENT);
  }
}

/** The Base Color record of a mask item, which holds its constant and its map; null when absent. */
const MaterialPaintLayerChannel *paint_mask_record(const MaterialPaintLayer &item)
{
  for (int i = 0; i < item.channels_num; i++) {
    if (item.channels[i].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      return &item.channels[i];
    }
  }
  return nullptr;
}

/** A generated map whose pixels were never written is drawn as a placeholder, not as empty. */
bool paint_image_is_blank(const Image &image)
{
  if (image.source != IMA_SRC_GENERATED) {
    return false;
  }
  const ImageTile *base_tile = BKE_image_get_tile(const_cast<Image *>(&image), 0);
  return base_tile != nullptr && base_tile->gen_type == IMA_GENTYPE_BLANK &&
         base_tile->gen_color[3] == 0.0f && !BKE_image_is_dirty(const_cast<Image *>(&image));
}

/**
 * The icon a mask slot shows: a switched-off mask says it does not shape the coverage; otherwise
 * its starting grey says whether it was added white or black. #BKE_paint_layers_mask_add keeps
 * that grey in the Base Color record's constant and painting writes the map instead, so
 * `value[0]` stays the starting value -- the one thing a thumbnail could not show. One function
 * for the base mask's slot and the mask items' slots, so the two cannot read a mask differently.
 */
static int paint_mask_icon(const MaterialPaintLayer &item, const bool enabled)
{
  if (!enabled) {
    return ICON_MOD_SUBSURF;
  }
  const MaterialPaintLayerChannel *record = paint_mask_record(item);
  /* A filter owns no Base Color record: it shapes the mask rather than laying a grey over it. */
  if (record == nullptr) {
    return ICON_MOD_MASK;
  }
  return record->value[0] > 0.5f ? ICON_CLIPUV_HLT : ICON_MOD_MASK;
}

/**
 * The mask slot of a row: always an icon, never a preview of its map, so it keeps saying that the
 * slot is the mask rather than the map it holds.
 */
StackRowPreview paint_mask_slot_build(const MaterialPaintLayer &item)
{
  StackRowPreview slot;
  slot.section_id = "MASK";
  slot.label = IFACE_("Mask");
  slot.icon = paint_mask_icon(item, (item.flag & MA_PAINT_LAYER_ENABLED) != 0);
  return slot;
}

}  // namespace

void outliner_stack_paint_row_ui_sync(Main &bmain,
                                      Scene &scene,
                                      ViewLayer &view_layer,
                                      SpaceOutliner &space_outliner,
                                      Material &material)
{
  if (space_outliner.outlinevis != SO_STACK_LAYERS || space_outliner.runtime == nullptr) {
    return;
  }
  const StackReadContext ctx{&bmain, &scene, &view_layer};
  if (outliner_stack_owner_get(ctx, space_outliner) != &material.id) {
    return;
  }
  /* The active marker itself, not #BKE_paint_layers_active_layer_get: a mask correction (a Curve
   * Patch mask item) would resolve to the row it hangs on, and that is not the row to select. */
  const bUUID active_marker = BKE_paint_layers_active_get(material);
  const MaterialPaintLayer *active = BKE_paint_layers_find(material, active_marker);
  if (active == nullptr) {
    return;
  }

  outliner_stack_rows_ensure(ctx, space_outliner, material.id);
  int ordinal = -1;
  for (const StackRow &row : space_outliner.runtime->stack_rows) {
    if (BLI_uuid_equal(row.stable_id, active->marker)) {
      ordinal = row.ordinal;
      break;
    }
  }
  if (ordinal < 0) {
    return;
  }
  /* No "already active" shortcut: the Outliner reads a row as active from the material's active
   * marker, which the activation has already set, so it would always bail out here while the UI
   * selection still sits on the previous row. */
  /* The identity-keyed UI state map carries the flags across the next tree build, and revealing
   * the attached section brings a correction row out from under its owner's content section. */
  stack_row_reveal_attached(space_outliner, ordinal);
  stack_row_ui_state_edit_select(ctx, space_outliner, material.id, ordinal);
}

namespace {

/**
 * The description's own problems -- a folder carrying a map, a Fill correction on Normal -- shown on
 * the row they are about, as a warning icon with the reason in its tooltip. \a correction is the
 * correction the issue is about, or nil for the layer's own.
 */
void paint_issue_slots_append(const Span<PaintLayersIssue> issues,
                              const bUUID &layer,
                              const bUUID &correction,
                              StackRow &row)
{
  for (const PaintLayersIssue &issue : issues) {
    if (!BLI_uuid_equal(issue.layer, layer) || !BLI_uuid_equal(issue.correction, correction)) {
      continue;
    }
    StackRowPreview warning;
    warning.icon = ICON_ERROR;
    warning.label = issue.text != nullptr ? issue.text : "";
    row.preview_slots.append(std::move(warning));
  }
}

/**
 * Point the value column at the per-channel opacity of \a layer for \a channel, so the header's
 * channel cursor edits that channel alone. Every row has one, a folder and a correction included,
 * whether or not it has a record for the channel.
 */
void paint_row_value_target_set(StackRow &row,
                                const Material &material,
                                const MaterialPaintLayer &layer,
                                const int channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return;
  }
  const MaterialPaintLayerChannelSettings &settings = layer.channel_settings[channel];
  row.value_ptr = RNA_pointer_create_discrete(
      &const_cast<Material &>(material).id,
      RNA_struct_find("MaterialPaintLayerChannelSettings"),
      const_cast<MaterialPaintLayerChannelSettings *>(&settings));
  row.value_prop = "opacity";
  row.value_inherited = (settings.opacity == 1.0f);
}

/**
 * Point the mode column at the per-channel blend of \a layer for \a channel, so the header's
 * channel cursor edits that channel alone. Inherit is the row's own blend, so the control dims
 * until the user picks another mode; the Normal channel's Mix is its combine rather than an
 * inherited value, so it stays bright.
 */
void paint_row_mode_target_set(StackRow &row,
                               const Material &material,
                               const MaterialPaintLayer &layer,
                               const int channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return;
  }
  const MaterialPaintLayerChannelSettings &settings = layer.channel_settings[channel];
  row.mode_ptr = RNA_pointer_create_discrete(
      &const_cast<Material &>(material).id,
      RNA_struct_find("MaterialPaintLayerChannelSettings"),
      const_cast<MaterialPaintLayerChannelSettings *>(&settings));
  row.mode_prop = "blend_type";
  row.mode_inherited = settings.blend < 0 && channel != PAINT_MATERIAL_CHANNEL_NORMAL;
}


/** Whether \a layer is an adjustment: a filter that is a row of the stack. */
static bool paint_layer_row_is_adjustment_view(const MaterialPaintLayer &layer)
{
  return layer.source == MA_PAINT_LAYER_SOURCE_FILTER && layer.role == MA_PAINT_LAYER_ROLE_LAYER;
}

/** The pair a paint row shows for its kind: the built-in icon and its Material Symbols glyph. */
struct PaintKindLook {
  int icon;
  /** Material Symbols codepoint, or 0 when the kind has no glyph and shows #icon alone. */
  uint32_t glyph;
};

/**
 * The role a row of one source plays. Only a Paint row looks different in the two -- the mask's
 * Add entry names it by the Grease Pencil icon -- but every row asks through this one function so
 * a role cannot drift from the Add menu that names it.
 */
enum class PaintLookRole {
  /** A layer's content, or a correction laid over it. */
  Content,
  /** A correction laid over the layer's mask. */
  MaskItem,
};

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
      return {ICON_IMAGE_ALPHA, paint_glyph::LEVELS};
    case MA_PAINT_LAYER_FILTER_HSL:
      return {ICON_COLOR, paint_glyph::HSL};
    case MA_PAINT_LAYER_FILTER_COLOR_ID:
      return {ICON_COLOR, paint_glyph::COLORIZE};
    case MA_PAINT_LAYER_FILTER_UV_ISLAND:
      return {ICON_UV_ISLANDSEL, paint_glyph::SELECT_ALL};
  }
  return {ICON_ARROW_LEFTRIGHT, 0};
}

/**
 * The icon and glyph every row and Add entry of \a source shows, in \a role. This is the one place
 * the pair is written: the rows, the effects and the Add menu all ask here, so a kind cannot read
 * one way in the menu and another in the stack.
 */
static PaintKindLook paint_look_for_source(const eMaterialPaintLayerSource source,
                                           const PaintLookRole role,
                                           const int16_t filter_subtype = 0)
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
      return {PAINT_GRADIENT_ICON, paint_glyph::GRADIENT};
    case MA_PAINT_LAYER_SOURCE_FILTER:
      return paint_filter_look(filter_subtype);
    case MA_PAINT_LAYER_SOURCE_CURVE_PATCH:
      return {ICON_CURVE_BEZCURVE, paint_glyph::LINE_CURVE};
    case MA_PAINT_LAYER_SOURCE_ANCHOR_POINT:
      return {ICON_EMPTY_AXIS, paint_glyph::ANCHOR};
    case MA_PAINT_LAYER_SOURCE_STACK:
      return {ICON_FILE_FOLDER, 0};
  }
  return {ICON_IMAGE_RGB, 0};
}

/** The look of an Anchor Point marker, whichever list it sits in. */
static PaintKindLook paint_look_for_anchor_point()
{
  return {ICON_EMPTY_AXIS, paint_glyph::ANCHOR};
}

/** The look of an Anchor Point reader: what it lays, not the marker it reads. */
static PaintKindLook paint_look_for_anchor_reader()
{
  return {ICON_LINKED, paint_glyph::CELL_TOWER};
}

/**
 * A filter the Add Effect menu offers. The vocabulary is the editor's (a name and a tooltip);
 * which filters exist is #eMaterialPaintLayerFilter. The icon and glyph come from
 * #paint_look_for_source, the same place a row asks, so the menu and the row cannot disagree.
 * #row_add reads the position back, so a new filter is one entry here and one in the kernel's kind
 * table.
 */
struct PaintFilterAddKind {
  const char *identifier;
  const char *name;
  const char *description;
  int16_t filter;
};

static const PaintFilterAddKind paint_filter_add_kinds[] = {
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
};

/**
 * The map-driven filters the Add Effect menu offers over the mask. They only exist in a mask (their
 * role is a mask item), so they have no correction or adjustment form like #paint_filter_add_kinds.
 */
static const PaintFilterAddKind paint_coverage_add_kinds[] = {
    {"FILTER_COLOR_ID",
     N_("Color ID"),
     "Keep the active layer's mask only where an ID Map matches the listed colors",
     MA_PAINT_LAYER_FILTER_COLOR_ID},
    {"FILTER_UV_ISLAND",
     N_("UV Island"),
     "Keep the active layer's mask only on the UV islands captured from the Image Editor",
     MA_PAINT_LAYER_FILTER_UV_ISLAND},
};

/** The row of one content correction, hung off the row \a owner_ordinal names. */
StackRow paint_correction_row_build(const Material &material,
                                    const MaterialPaintLayer &effect,
                                    const bUUID &owner_marker,
                                    const Span<PaintLayersIssue> issues,
                                    const int depth,
                                    const int owner_ordinal,
                                    const int ordinal,
                                    const int channel)
{
  StackRow row;
  row.ordinal = int16_t(ordinal);
  row.depth = depth;
  row.parent_ordinal = int16_t(owner_ordinal);
  row.parent_section_id = "CHANNELS";
  row.stable_id = effect.marker;
  row.enabled = (effect.flag & MA_PAINT_LAYER_ENABLED) != 0;
  row.locked = BKE_paint_layers_is_locked(effect);
  row.supported = true;
  const bool fill = effect.source == MA_PAINT_LAYER_SOURCE_CONSTANT;
  const bool from_material = effect.source == MA_PAINT_LAYER_SOURCE_MATERIAL;
  const bool curve_patch = effect.source == MA_PAINT_LAYER_SOURCE_CURVE_PATCH;
  row.name = effect.name[0] != '\0' ?
                 effect.name :
                 (from_material ? "Material Correction" :
                                  (curve_patch ? "Curve Patch" :
                                                 (fill ? "Correction Fill" : "Correction Paint")));
  row.name_buffer = const_cast<char *>(effect.name);
  const bool filter = effect.source == MA_PAINT_LAYER_SOURCE_FILTER;
  const PaintKindLook look = paint_look_for_source(
      eMaterialPaintLayerSource(effect.source), PaintLookRole::Content, effect.kind_subtype);
  row.icon = look.icon;
  row.glyph = look.glyph;
  if (curve_patch) {
    /* The effect's action is its tool entrance, the same one the row's own action icon is. */
    row.action_icon = ICON_CURVE_BEZCURVE;
    row.action_tooltip = TIP_("Edit Curves: enter the Curve Patch tool of this effect");
  }
  if (BKE_paint_layers_is_anchor_point(effect)) {
    /* A marker is a place, not a correction: it publishes a slice and has no opacity, blend or map
     * of its own, so it is a bare compact row with the icon of its Add entry. */
    row.name = effect.name[0] != '\0' ? effect.name : "Anchor Point";
    const PaintKindLook anchor_look = paint_look_for_anchor_point();
    row.icon = anchor_look.icon;
    row.glyph = anchor_look.glyph;
    row.compact = true;
    paint_issue_slots_append(issues, owner_marker, effect.marker, row);
    return row;
  }
  const bool reads_anchor_point = BKE_paint_layers_reads_anchor_point(effect);
  if (reads_anchor_point) {
    row.name = effect.name[0] != '\0' ? effect.name : "Anchor Point Reader";
    const PaintKindLook reader_look = paint_look_for_anchor_reader();
    row.icon = reader_look.icon;
    row.glyph = reader_look.glyph;
  }
  /* A correction reads as a setting of its layer rather than as a member of the stack. */
  row.compact = true;

  PointerRNA effect_ptr = RNA_pointer_create_discrete(
      &const_cast<Material &>(material).id,
      RNA_struct_find("MaterialPaintLayer"),
      const_cast<MaterialPaintLayer *>(&effect));
  row.value_ptr = effect_ptr;
  row.value_prop = "opacity";
  row.value_inherited = (effect.opacity == 1.0f);
  paint_row_value_target_set(row, material, effect, channel);
  if (filter) {
    /* A filter has no map to paint into and no blend: only its opacity and its issues. */
    paint_issue_slots_append(issues, owner_marker, effect.marker, row);
    return row;
  }
  /* The mode column edits the blend of this channel alone, like the value column above. */
  paint_row_mode_target_set(row, material, effect, channel);
  if (from_material || reads_anchor_point) {
    /* It shows what its source shows and owns no map: nothing to preview or switch the brush to,
     * as for a Material layer. A reader shows the slice its point publishes. */
    paint_issue_slots_append(issues, owner_marker, effect.marker, row);
    return row;
  }

  /* The slot names the correction's kind rather than previewing its first map, as a Paint layer's
   * slot does: a painted correction still reads as the brush, fill or generator it is, not as the
   * texture it holds. A Fill correction that lays a colour shows its swatch instead, with the same
   * rule when the Base Color record is gone or switched off (the correction then lays no colour). */
  StackRowPreview slot;
  slot.section_id = "CHANNELS";
  slot.icon = look.icon;
  slot.glyph = look.glyph;
  const MaterialPaintLayerChannel *base = nullptr;
  for (int c = 0; c < effect.channels_num; c++) {
    if (effect.channels[c].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      base = &effect.channels[c];
    }
  }
  if (fill && BKE_paint_layers_base_color_active(effect) && base->image == nullptr &&
      BKE_paint_layers_channel_in_set(material, PAINT_MATERIAL_CHANNEL_BASE_COLOR))
  {
    slot.is_color_swatch = true;
    BKE_paint_layers_base_color_get(effect, slot.color);
    slot.label = IFACE_("Fill Color");
  }
  row.preview_slots.append(std::move(slot));
  StackContentSection channels;
  channels.identifier = "CHANNELS";
  channels.name = IFACE_("Channel Textures");
  row.content_sections.append(std::move(channels));
  paint_issue_slots_append(issues, owner_marker, effect.marker, row);
  return row;
}

/**
 * The row of one mask correction, hung off the row \a owner_ordinal names under its mask section: it
 * is listed while the mask is the content on show, the way a content correction is while the
 * channels are.
 */
StackRow paint_mask_item_row_build(const Material &material,
                                   const MaterialPaintLayer &item,
                                   const bUUID &owner_marker,
                                   const int depth,
                                   const int owner_ordinal,
                                   const int ordinal,
                                   const Span<PaintLayersIssue> issues)
{
  const bool fill = item.source == MA_PAINT_LAYER_SOURCE_CONSTANT;
  const bool from_material = item.source == MA_PAINT_LAYER_SOURCE_MATERIAL;
  StackRow row;
  row.ordinal = int16_t(ordinal);
  row.depth = depth;
  row.parent_ordinal = int16_t(owner_ordinal);
  row.parent_section_id = "MASK";
  row.stable_id = item.marker;
  row.enabled = (item.flag & MA_PAINT_LAYER_ENABLED) != 0;
  row.locked = BKE_paint_layers_is_locked(item);
  row.supported = true;
  row.name = item.name[0] != '\0' ?
                 item.name :
                 (from_material ? "Material Mask" : (fill ? "Mask Fill" : "Mask Paint"));
  row.name_buffer = const_cast<char *>(item.name);
  /* The icons and glyphs the Add Effect menu names these roles by, through the one look: a Mask
   * Paint Correction is the Grease Pencil paint icon there, so the row it makes reads the same. */
  const PaintKindLook look = paint_look_for_source(
      eMaterialPaintLayerSource(item.source), PaintLookRole::MaskItem, item.kind_subtype);
  row.icon = look.icon;
  row.glyph = look.glyph;
  if (BKE_paint_layers_is_anchor_point(item)) {
    row.name = item.name[0] != '\0' ? item.name : "Anchor Point";
    const PaintKindLook anchor_look = paint_look_for_anchor_point();
    row.icon = anchor_look.icon;
    row.glyph = anchor_look.glyph;
    row.compact = true;
    paint_issue_slots_append(issues, owner_marker, item.marker, row);
    return row;
  }
  const bool reads_anchor_point = BKE_paint_layers_reads_anchor_point(item);
  if (reads_anchor_point) {
    row.name = item.name[0] != '\0' ? item.name : "Anchor Point Reader";
    const PaintKindLook reader_look = paint_look_for_anchor_reader();
    row.icon = reader_look.icon;
    row.glyph = reader_look.glyph;
  }
  /* A correction reads as a setting of its mask rather than as a member of the stack. */
  row.compact = true;

  PointerRNA item_ptr = RNA_pointer_create_discrete(
      &const_cast<Material &>(material).id,
      RNA_struct_find("MaterialPaintLayer"),
      const_cast<MaterialPaintLayer *>(&item));
  row.value_ptr = item_ptr;
  row.value_prop = "opacity";
  row.value_inherited = (item.opacity == 1.0f);
  if (item.source == MA_PAINT_LAYER_SOURCE_FILTER) {
    /* A filter in the mask changes its coverage: no map to paint, no blend, no channels. Its look
     * already names the filter by the icon and glyph its Add entry uses. */
    paint_issue_slots_append(issues, owner_marker, item.marker, row);
    return row;
  }
  row.mode_ptr = item_ptr;
  row.mode_prop = "blend_type";
  row.mode_inherited = (item.blend == MA_PAINT_LAYER_BLEND_MIX);
  if (from_material || reads_anchor_point) {
    /* Its grey is its source's Base Color (or the slice it reads) and it owns no map: nothing to
     * preview or paint. A reader's issues (a bad reference, a UDIM slice) are shown here. */
    paint_issue_slots_append(issues, owner_marker, item.marker, row);
    return row;
  }

  /* The slot switches the brush to the mask. A Fill is its grey while it has no map, as a Fill layer
   * is its colour; a Paint one, and a Fill once painted, is the mask icon the base's slot shows. */
  StackRowPreview slot;
  slot.section_id = "MASK";
  slot.label = IFACE_("Mask");
  const MaterialPaintLayerChannel *record = paint_mask_record(item);
  if (fill && record != nullptr && record->image == nullptr) {
    const float gray = BKE_paint_layers_mask_constant(item);
    slot.is_color_swatch = true;
    slot.color[0] = slot.color[1] = slot.color[2] = gray;
    slot.color[3] = 1.0f;
  }
  slot.icon = paint_mask_icon(item, row.enabled);
  row.preview_slots.append(std::move(slot));
  StackContentSection mask_section;
  mask_section.identifier = "MASK";
  mask_section.name = IFACE_("Mask Content");
  row.content_sections.append(std::move(mask_section));
  return row;
}

/**
 * The constants a Fill's sphere preview shades with: the flat colour the swatch already shows,
 * plus the metallic and roughness a flat colour cannot. A channel that carries a map hands the
 * map's session UID instead, so the sphere shades with the texture the layer lays down rather than
 * with the constant the record keeps beside it.
 */
static void paint_fill_sphere_read(const MaterialPaintLayer &layer,
                                   float &r_metallic,
                                   float &r_roughness,
                                   uint32_t r_map_uid[3])
{
  r_metallic = 0.0f;
  r_roughness = 0.5f;
  r_map_uid[0] = r_map_uid[1] = r_map_uid[2] = 0;
  for (int c = 0; c < layer.channels_num; c++) {
    const MaterialPaintLayerChannel &record = layer.channels[c];
    switch (record.channel) {
      case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
        r_map_uid[0] = record.image != nullptr ? record.image->id.session_uid : 0;
        break;
      case PAINT_MATERIAL_CHANNEL_METALLIC:
        r_metallic = record.value[0];
        r_map_uid[1] = record.image != nullptr ? record.image->id.session_uid : 0;
        break;
      case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
        r_roughness = record.value[0];
        r_map_uid[2] = record.image != nullptr ? record.image->id.session_uid : 0;
        break;
      default:
        break;
    }
  }
}

/**
 * Everything the Fill's sphere preview shows, folded into the one value its slot carries: an edit
 * anywhere in it -- a dragged colour, a retuned roughness -- re-renders the sphere. The blend and
 * the opacity are left out on purpose: they compose the row over the stack and do not change the
 * fill itself, and folding them in would re-render the sphere for every slider drag that leaves
 * it looking the same. The exact float bits are folded rather than their decimal values, so two
 * constants cannot fold alike by rounding.
 */
static uint64_t paint_fill_sphere_hash(const MaterialPaintLayer &layer)
{
  uint64_t hash = 0;
  const auto mix = [&hash](const uint64_t value) {
    hash = (hash << 27) + (hash >> 5) + value + 0x9e3779b97f4a7c15ull;
  };
  const auto mix_float = [&mix](const float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    mix(bits);
  };
  for (int c = 0; c < layer.channels_num; c++) {
    const MaterialPaintLayerChannel &record = layer.channels[c];
    mix((uint64_t(uint8_t(record.channel)) << 8) | uint8_t(record.state));
    mix_float(record.value[0]);
    mix_float(record.value[1]);
    mix_float(record.value[2]);
    mix_float(record.value[3]);
    mix(record.image != nullptr ? record.image->id.session_uid : 0);
  }
  return hash;
}

/**
 * The neutral base a colourless Fill's sphere preview shades with: the row lays no Base Color of
 * its own, but its other channels still shade the material the sphere stands for.
 */
constexpr float PAINT_FILL_NEUTRAL_COLOR = 0.8f;

/**
 * The content slot of a Fill row: its own colour as a swatch where it lays one, otherwise the
 * sphere its remaining channels shade. Null when the Fill lays neither -- no colour and no channel
 * records -- so the row shows its plain icon and nothing to open.
 */
static std::optional<StackRowPreview> paint_fill_slot_build(const Material &material,
                                                            const MaterialPaintLayer &layer)
{
  const bool lays_color = BKE_paint_layers_base_color_active(layer) &&
                          BKE_paint_layers_channel_in_set(
                              material, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  if (!lays_color && layer.channels_num == 0) {
    return std::nullopt;
  }
  StackRowPreview fill_slot;
  fill_slot.section_id = "CHANNELS";
  const PaintKindLook look = paint_look_for_source(MA_PAINT_LAYER_SOURCE_CONSTANT,
                                                   PaintLookRole::Content);
  /* The badge the draw puts in the slot's corner: a Fill reads as a Fill whichever of its two
   * looks -- the colour swatch or the sphere -- answers first. */
  fill_slot.glyph = look.glyph;
  if (lays_color) {
    fill_slot.is_color_swatch = true;
    fill_slot.icon = look.icon;
    BKE_paint_layers_base_color_get(layer, fill_slot.color);
    fill_slot.label = IFACE_("Fill Color");
  }
  else {
    fill_slot.color[0] = fill_slot.color[1] = fill_slot.color[2] = PAINT_FILL_NEUTRAL_COLOR;
    fill_slot.color[3] = 1.0f;
    fill_slot.label = IFACE_("Fill Preview");
  }
  /* The sphere is the constants shaded -- metallic and roughness included, from the flat values or
   * their maps -- arriving from the preview render. A swatch underneath it is the fill's instant
   * answer; a Fill with no colour has none and shows the render alone. */
  paint_fill_sphere_read(
      layer, fill_slot.sphere_metallic, fill_slot.sphere_roughness, fill_slot.sphere_map_uid);
  fill_slot.render_kind = StackRowPreviewRender::Sphere;
  fill_slot.render_hash = paint_fill_sphere_hash(layer);
  return fill_slot;
}

/**
 * Everything a Material row's sphere preview shows, folded into the one value its slot carries:
 * the source's identity and the hash of its whole graph, so an edit of it re-renders; each
 * channel's resolution with the map or the constant it resolved to; and -- where a channel's
 * answer is the bake's, the only case that is -- the bake's stamps, so a re-bake landing new maps
 * re-renders the sphere off them.
 */
static uint64_t paint_material_sphere_hash(const Material &source,
                                           const MaterialSourceResolve &resolve,
                                           const MaterialPaintLayer &layer)
{
  uint64_t hash = 0;
  const auto mix = [&hash](const uint64_t value) {
    hash = (hash << 27) + (hash >> 5) + value + 0x9e3779b97f4a7c15ull;
  };
  mix(source.id.session_uid);
  mix(BKE_paint_layers_source_material_tree_hash_cached(source));
  for (const int channel :
       {PAINT_MATERIAL_CHANNEL_BASE_COLOR, PAINT_MATERIAL_CHANNEL_METALLIC,
        PAINT_MATERIAL_CHANNEL_ROUGHNESS})
  {
    const ChannelResolution resolution = resolve.channels[channel];
    mix(uint8_t(resolution));
    if (resolution == ChannelResolution::Constant) {
      uint64_t constant_bits[2];
      memcpy(constant_bits, &resolve.constants[channel], sizeof(constant_bits));
      mix(constant_bits[0]);
      mix(constant_bits[1]);
    }
    else if (resolution == ChannelResolution::Image) {
      const Image *map = resolve.images[channel].image;
      mix(map != nullptr ? map->id.session_uid : 0);
    }
    else if (resolution == ChannelResolution::Baked && layer.bake != nullptr) {
      mix(layer.bake->hash[0] | (uint64_t(layer.bake->hash[1]) << 32));
      mix(layer.bake->source_hash[0] | (uint64_t(layer.bake->source_hash[1]) << 32));
      mix(uint32_t(layer.bake->size));
      const Image *map = layer.bake->images[channel];
      mix(map != nullptr ? map->id.session_uid : 0);
    }
  }
  return hash;
}

/**
 * Append the rows of \a list, and recursively of every folder in it, depth first. A folder's row
 * is appended after what it holds: the tree is built walking the rows from the last one down, and a
 * child is only hung off a folder already made. The ordinals keep the walk order -- rows are looked
 * up by ordinal, never by position -- which is the one #BKE_paint_layers_foreach defines.
 */
void paint_stack_rows_append(const Material &material,
                             const ListBaseT<MaterialPaintLayer> &list,
                             const Span<PaintLayersIssue> issues,
                             const int depth,
                             const int parent_ordinal,
                             const int channel,
                             int &ordinal,
                             bool &overflow,
                             Vector<StackRow> &r_rows)
{
  for (const MaterialPaintLayer &layer : list) {
    if (overflow) {
      return;
    }
    if (ordinal > STACK_ROW_ORDINAL_MAX) {
      overflow = true;
      return;
    }
    const bool folder = BKE_paint_layers_is_folder(layer);
    StackRow row;
    row.ordinal = int16_t(ordinal++);
    row.depth = depth;
    row.parent_ordinal = int16_t(parent_ordinal);
    row.stable_id = layer.marker;
    row.can_hold_children = folder;
    row.has_children = BKE_paint_layers_has_children(layer);
    row.enabled = (layer.flag & MA_PAINT_LAYER_ENABLED) != 0;
    row.locked = BKE_paint_layers_is_locked(layer);
    row.supported = true;
    row.name = layer.name[0] != '\0' ? layer.name : (folder ? "Folder" : "Layer");
    row.name_buffer = const_cast<char *>(layer.name);
    row.color_tag = layer.color_tag;
    /* An adjustment is a filter of the stack below it: no map, no channel slot, only its settings. */
    const bool adjustment = paint_layer_row_is_adjustment_view(layer);
    const PaintKindLook look = paint_look_for_source(
        eMaterialPaintLayerSource(layer.source), PaintLookRole::Content, layer.kind_subtype);
    row.icon = folder ? ICON_FILE_FOLDER : look.icon;
    /* A folder keeps the built-in folder icon; only the layer kinds that draw content use a
     * glyph. */
    row.glyph = folder ? 0 : look.glyph;
    if (BKE_paint_layers_reads_anchor_point(layer)) {
      /* A reader is not the marker: it lays the slice, so it takes the reader glyph. */
      const PaintKindLook reader_look = paint_look_for_anchor_reader();
      row.icon = reader_look.icon;
      row.glyph = reader_look.glyph;
    }
    /* Only layers and folders limit where they show; an adjustment acts on what is below it for
     * every object alike, and corrections and mask items are not rows of this list. */
    if (!adjustment && layer.role == MA_PAINT_LAYER_ROLE_LAYER) {
      row.action_icon = ICON_SELECT_SET;
      row.action_tooltip = TIP_("Mesh Mask: choose the objects or UDIM tiles this layer shows on");
    }

    /* The value column edits the row's opacity; a value left at its default reads as inherited
     * rather than deliberately set, so the control dims until the user moves it. */
    PointerRNA layer_ptr = RNA_pointer_create_discrete(
        &const_cast<Material &>(material).id,
        RNA_struct_find("MaterialPaintLayer"),
        const_cast<MaterialPaintLayer *>(&layer));
    row.value_ptr = layer_ptr;
    row.value_prop = "opacity";
    row.value_inherited = (layer.opacity == 1.0f);
    paint_row_value_target_set(row, material, layer, channel);

    /* The mode column edits how the row blends over what is below it in the selected channel;
     * Inherit is the row's own blend, so the control dims until the user picks another mode. */
    paint_row_mode_target_set(row, material, layer, channel);

    if (folder) {
      /* A folder is what it holds: it has no map, so no channel slot. Its content section is the
       * one the brush switches back to from the mask. */
      StackContentSection content;
      content.identifier = "CHANNELS";
      content.name = IFACE_("Content");
      row.content_sections.append(std::move(content));
    }
    else if (adjustment || layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL ||
             layer.source == MA_PAINT_LAYER_SOURCE_GENERATOR)
    {
      /* A Gradient row has no map, but its slot shows the generator's glyph so the row reads by
       * its kind; an adjustment has no content and no slot. Both take corrections, which are
       * listed under the content section, so they have it with or without a mask. A Material row's
       * preview is what the row paints, resolved the same way the
       * paint system itself reads the source: a plain constant or a plain texture of the source's
       * own shades a sphere, and so does the bake where the source is a graph. A graph whose bake
       * is missing or no longer describes the source -- Auto leaves it to the live shader -- has no
       * map to shade with, and the colour fields would show what the graph no longer is, so the
       * source's own preview answers instead; its change flag follows the graph's edits. The flat
       * colour answers wherever neither can; without a source, the placeholder below. */
      if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer.material != nullptr) {
        StackRowPreview material_slot;
        material_slot.section_id = "CHANNELS";
        material_slot.label = layer.material->id.name + 2;
        /* The badge the draw puts in the sphere's corner names the row's kind, as the Fill's does:
         * the sphere says what the material shows, the icon says it is a Material layer. */
        const PaintKindLook material_look = paint_look_for_source(
            MA_PAINT_LAYER_SOURCE_MATERIAL, PaintLookRole::Content);
        material_slot.icon = material_look.icon;
        material_slot.is_color_swatch = true;
        /* The remembered answer: the rows are rebuilt on every stack redraw. */
        const std::shared_ptr<const MaterialSourceResolve> resolve_shared =
            BKE_paint_layers_source_resolve(*layer.material);
        const MaterialSourceResolve resolve = resolve_shared ?
                                                  *resolve_shared :
                                                  BKE_paint_material_source_resolve(layer.material);
        if (resolve.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR] == ChannelResolution::Constant) {
          const float4 &base = resolve.constants[PAINT_MATERIAL_CHANNEL_BASE_COLOR];
          material_slot.color[0] = base[0];
          material_slot.color[1] = base[1];
          material_slot.color[2] = base[2];
          material_slot.color[3] = base[3];
        }
        else {
          /* The source's own RGB fields are display-referred; the slot's colour is scene linear
           * like every slot colour. */
          const float source_color[4] = {
              layer.material->r, layer.material->g, layer.material->b, layer.material->a};
          srgb_to_linearrgb_v4(material_slot.color, source_color);
        }
        material_slot.sphere_metallic =
            (resolve.channels[PAINT_MATERIAL_CHANNEL_METALLIC] == ChannelResolution::Constant) ?
                resolve.constants[PAINT_MATERIAL_CHANNEL_METALLIC][0] :
                0.0f;
        material_slot.sphere_roughness =
            (resolve.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS] == ChannelResolution::Constant) ?
                resolve.constants[PAINT_MATERIAL_CHANNEL_ROUGHNESS][0] :
                0.5f;
        /* The maps the sphere shades with: the source's own texture where the source is one -- the
         * system reads it directly and never bakes it -- and the bake's map where the source is a
         * graph, the one case the row is baked for. */
        bool graph_without_map = false;
        for (const int channel :
             {PAINT_MATERIAL_CHANNEL_BASE_COLOR, PAINT_MATERIAL_CHANNEL_METALLIC,
              PAINT_MATERIAL_CHANNEL_ROUGHNESS})
        {
          const Image *map = nullptr;
          if (resolve.channels[channel] == ChannelResolution::Image) {
            map = resolve.images[channel].image;
          }
          else if (resolve.channels[channel] == ChannelResolution::Baked) {
            map = layer.bake != nullptr ? layer.bake->images[channel] : nullptr;
            graph_without_map |= (map == nullptr);
          }
          material_slot.sphere_map_uid[channel] = map != nullptr ? map->id.session_uid : 0;
        }
        const bool graph_channel = ELEM(ChannelResolution::Baked,
                                        resolve.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR],
                                        resolve.channels[PAINT_MATERIAL_CHANNEL_METALLIC],
                                        resolve.channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS]);
        if (graph_channel &&
            (graph_without_map || BKE_paint_layers_material_row_stale(material, layer)))
        {
          material_slot.render_kind = StackRowPreviewRender::SourceRender;
          material_slot.id_uid = layer.material->id.session_uid;
          material_slot.id_type = ID_MA;
          /* The source alone is what is rendered, so only its graph decides a new render. */
          material_slot.render_hash = (uint64_t(layer.material->id.session_uid) << 1) ^
                                      BKE_paint_layers_source_material_tree_hash_cached(
                                          *layer.material);
        }
        else if (material_slot.sphere_map_uid[PAINT_MATERIAL_CHANNEL_BASE_COLOR] != 0 ||
                 resolve.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR] == ChannelResolution::Constant)
        {
          material_slot.render_kind = StackRowPreviewRender::Sphere;
          material_slot.render_hash = paint_material_sphere_hash(*layer.material, resolve, layer);
        }
        row.preview_slots.append(std::move(material_slot));
      }
      if (layer.source == MA_PAINT_LAYER_SOURCE_GENERATOR) {
        StackRowPreview gradient_slot;
        const PaintKindLook gradient_look = paint_look_for_source(
            MA_PAINT_LAYER_SOURCE_GENERATOR, PaintLookRole::Content);
        gradient_slot.section_id = "CHANNELS";
        gradient_slot.glyph = gradient_look.glyph;
        gradient_slot.icon = gradient_look.icon;
        gradient_slot.label = IFACE_("Gradient");
        row.preview_slots.append(std::move(gradient_slot));
      }
      if (BKE_paint_layers_mask_base(layer) != nullptr ||
          layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL ||
          layer.source == MA_PAINT_LAYER_SOURCE_GENERATOR)
      {
        StackContentSection content;
        content.identifier = "CHANNELS";
        content.name = IFACE_("Content");
        row.content_sections.append(std::move(content));
      }
    }
    else if (layer.source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
      /* A Fill with a colour of its own is that colour: one swatch with the bucket drawn over it.
       * With the Base Color record removed, switched off, or outside the material's channel set
       * the row lays no colour, but its other channels may still carry values and maps -- an image
       * dropped into Roughness, say. Those shade a material, so the sphere is what previews them:
       * drawing the map itself as the row's colour would say it lays a colour it does not. */
      if (std::optional<StackRowPreview> fill_slot = paint_fill_slot_build(material, layer)) {
        row.preview_slots.append(std::move(*fill_slot));
      }
    }
    else {
      /* A Paint layer's content slot keeps the row's own glyph -- the brush it paints with -- rather
       * than turning into a thumbnail of the first map once there is paint in it: the row reads as
       * the kind of layer it is, not as its texture. The slot still names the content section, so a
       * click switches the brush to it. */
      StackRowPreview channels_slot;
      channels_slot.section_id = "CHANNELS";
      channels_slot.glyph = row.glyph;
      channels_slot.icon = row.icon;
      row.preview_slots.append(std::move(channels_slot));
    }

    if (!folder && !adjustment && layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL &&
        layer.source != MA_PAINT_LAYER_SOURCE_GENERATOR)
    {
      /* The channel maps are the layer's internals rather than rows of their own; the section
       * stays so the brush can switch to the content and later rows can attach to it. */
      StackContentSection channels;
      channels.identifier = "CHANNELS";
      channels.name = IFACE_("Channel Textures");
      row.content_sections.append(std::move(channels));
    }

    /* The mask: a slot of its own, whose click switches the brush to it, and the section that slot
     * reveals. Its switch rides on the row, so a switched-off mask is kept and listed. */
    if (const MaterialPaintLayer *mask = BKE_paint_layers_mask_base(layer)) {
      if (folder || adjustment || layer.source == MA_PAINT_LAYER_SOURCE_GENERATOR ||
          (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer.material == nullptr))
      {
        /* The slot that switches back to the content, ahead of the mask's. The row icon stays in
         * front of it, as it does for a folder with no mask; these rows have no map of their own to
         * show in a slot, so without this one the brush could not leave the mask. A Material row
         * with a source switches with its own preview slot, which opens the same section. */
        StackRowPreview content_slot;
        content_slot.section_id = "CHANNELS";
        content_slot.icon = ICON_BRUSHES_ALL;
        content_slot.keeps_row_icon = true;
        content_slot.label = IFACE_("Content");
        row.preview_slots.append(std::move(content_slot));
      }
      row.preview_slots.append(paint_mask_slot_build(*mask));
      row.aux_enabled = (mask->flag & MA_PAINT_LAYER_ENABLED) != 0;
      StackContentSection mask_section;
      mask_section.identifier = "MASK";
      mask_section.name = IFACE_("Mask Content");
      row.content_sections.append(std::move(mask_section));
    }

    paint_issue_slots_append(issues, layer.marker, bUUID{}, row);

    const int row_ordinal = row.ordinal;
    /* The corrections are dense settings of their layer: rows attached to its content section,
     * numbered right after it (see #paint_rows_walk). */
    for (const MaterialPaintLayer &effect : layer.effects) {
      if (ordinal > STACK_ROW_ORDINAL_MAX) {
        overflow = true;
        return;
      }
      r_rows.append(paint_correction_row_build(
          material, effect, layer.marker, issues, depth + 1, row_ordinal, ordinal++, channel));
    }
    /* The mask's corrections follow, numbered the same way: the base is the mask's own slot. */
    for (const MaterialPaintLayer &item : layer.mask_stack) {
      if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
        continue;
      }
      if (ordinal > STACK_ROW_ORDINAL_MAX) {
        overflow = true;
        return;
      }
      r_rows.append(paint_mask_item_row_build(
          material, item, layer.marker, depth + 1, row_ordinal, ordinal++, issues));
    }
    if (folder) {
      paint_stack_rows_append(
          material, layer.children, issues, depth + 1, row_ordinal, channel, ordinal, overflow, r_rows);
    }
    r_rows.append(std::move(row));
  }
}

}  // namespace

void paint_stack_rows_from_description(const Material &material,
                                       const int channel,
                                       Vector<StackRow> &r_rows)
{

  int ordinal = 0;
  bool overflow = false;
  /* Built once: it is cheap, but a row would otherwise ask the whole description per row. */
  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(material, issues);
  paint_stack_rows_append(
      material, material.paint_layers, issues, 0, -1, channel, ordinal, overflow, r_rows);
  if (overflow) {
    StackRow overflow_row;
    overflow_row.ordinal = STACK_ROW_ORDINAL_MAX + 1;
    overflow_row.supported = false;
    overflow_row.unsupported_reason = "Stack is too large to display";
    overflow_row.icon = ICON_IMAGE_RGB;
    r_rows.append(std::move(overflow_row));
  }
}

namespace {

/**
 * Visit every row the Outliner lists in the order it numbers them: a row, then its corrections, then
 * its mask's, then what a folder holds. #paint_stack_rows_append hands out the ordinals in this same order, so
 * the two cannot disagree about which row an ordinal names. Stops when \a fn returns false.
 *
 * \a ListT is the list of rows or its const form, so a caller that needs to edit the row it finds
 * walks the mutable list instead of casting the constness away afterwards.
 */
template<typename ListT, typename Fn> bool paint_rows_walk(ListT &list, const Fn &fn)
{
  using RowT = std::remove_reference_t<decltype(*list.begin())>;
  for (RowT &layer : list) {
    if (!fn(layer)) {
      return false;
    }
    for (RowT &effect : layer.effects) {
      if (!fn(effect)) {
        return false;
      }
    }
    for (RowT &item : layer.mask_stack) {
      if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
        continue;
      }
      if (!fn(item)) {
        return false;
      }
    }
    if (BKE_paint_layers_is_folder(layer) && !paint_rows_walk(layer.children, fn)) {
      return false;
    }
  }
  return true;
}

}  // namespace

MaterialPaintLayer *paint_description_row_for_ordinal(Material &material, const int ordinal)
{
  if (ordinal < 0) {
    return nullptr;
  }
  /* The ordinal is the row's place in the depth-first walk the row builder numbers by. */
  int index = 0;
  MaterialPaintLayer *found = nullptr;
  paint_rows_walk(material.paint_layers, [&](MaterialPaintLayer &layer) -> bool {
    if (index == ordinal) {
      found = &layer;
      return false;
    }
    index++;
    return true;
  });
  return found;
}

int paint_layers_ordinal_of(const Material &material, const MaterialPaintLayer *target)
{
  if (target == nullptr) {
    return -1;
  }
  int index = 0;
  int found = -1;
  paint_rows_walk(material.paint_layers, [&](const MaterialPaintLayer &layer) -> bool {
    if (&layer == target) {
      found = index;
      return false;
    }
    index++;
    return true;
  });
  return found;
}

bool paint_layers_edit_rename(Material &material, const int ordinal, const char *name)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  return layer != nullptr && BKE_paint_layers_rename(material, layer, name);
}

int paint_layers_edit_duplicate(Main &bmain, Material &material, const int ordinal)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  if (layer == nullptr) {
    return -1;
  }
  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(bmain, material, layer);
  return (copy != nullptr) ? paint_layers_ordinal_of(material, copy) : -1;
}

bool paint_layers_edit_color_tag(Material &material, const int ordinal, const int color_tag)
{
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
  return layer != nullptr && BKE_paint_layers_set_color_tag(material, layer, color_tag);
}

bool paint_layers_edit_target_clear(Material &material)
{
  if (BLI_uuid_is_nil(material.active_layer_marker)) {
    return false;
  }
  BKE_paint_layers_active_set(material, bUUID{});
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add kinds
 * \{ */

/**
 * One kind of the Add menu: what the user sees, and what #row_add asks the kernel for.
 *
 * The table is the only place that knows the kinds. A kind's index in it is the number scripts and
 * the Add operator pass to #row_add, so a new kind is appended at the end and nothing above moves.
 * The Effects menu folds the two roles of one thing into one line, and the fields that say how
 * -- #mask_twin, #is_mask_twin, #mask_only and #menu_name -- live here, so a new correction is
 * still one entry and cannot be added to one role only.
 */
struct PaintAddKindEntry {
  StackAddKindInfo info;
  PaintLayerAddKind kind = PaintLayerAddKind::Paint;
  /** The #eMaterialPaintLayerFilter of a filter, adjustment or mask filter kind; else unused. */
  int16_t filter = 0;
  /**
   * Index of the entry that lays this effect over the mask, or -1. Resolved while the table is
   * built from the mask kind the content entry names; the mask entry gets #is_mask_twin.
   */
  int mask_twin = -1;
  /** Whether this entry is the mask form the Effects menu folds into its content entry. */
  bool is_mask_twin = false;
  /** Whether the kind exists only in the mask, so the menu offers it only while a mask slot is
   * the one showing. */
  bool mask_only = false;
  /** The function word the Effects menu uses for this entry; null keeps #info.name. */
  const char *menu_name = nullptr;
};

static Span<PaintAddKindEntry> paint_add_kind_entries()
{
  static const Vector<PaintAddKindEntry> entries = []() {
    Vector<PaintAddKindEntry> table;
    /* The mask kinds named by content entries, and the rows that named them. Resolved to indexes
     * once every entry exists, so a pair can be written without depending on the order. */
    Vector<std::optional<PaintLayerAddKind>> twin_kinds;
    Vector<int> twin_rows;
    const auto add = [&](StackAddKindInfo info,
                         const PaintLayerAddKind kind,
                         const int16_t filter = 0,
                         const std::optional<PaintLayerAddKind> mask_twin = std::nullopt,
                         const char *menu_name = nullptr,
                         const bool mask_only = false) {
      PaintAddKindEntry entry;
      entry.info = std::move(info);
      entry.kind = kind;
      entry.filter = filter;
      entry.menu_name = menu_name;
      entry.mask_only = mask_only;
      table.append(std::move(entry));
      if (mask_twin.has_value()) {
        twin_kinds.append(mask_twin);
        twin_rows.append(int(table.size()) - 1);
      }
    };
    const auto effect = [](StackAddKindInfo info) {
      info.is_effect = true;
      return info;
    };
    /* The icon and the glyph are the look's, never written here: an Add entry and the row it makes
     * must read the same. */
    const auto with_look = [](StackAddKindInfo info, const PaintKindLook look) {
      info.icon = look.icon;
      info.glyph = look.glyph;
      return info;
    };
    const auto with_color = [](StackAddKindInfo info) {
      info.takes_color = true;
      return info;
    };
    /* A Fill may also start empty: a row with no channels, to assign them to. */
    const auto with_optional_color = [](StackAddKindInfo info) {
      info.takes_color = true;
      info.color_optional = true;
      return info;
    };
    /* A kind made from another material, picked from the ID Browser popover with the Material
     * asset libraries. */
    const auto from_material = [](StackAddKindInfo info, const char *browse_prop) {
      info.source_id_type = ID_MA;
      info.source_browse_prop = browse_prop;
      return info;
    };

    add(with_look({"PAINT",
                   N_("Paint Layer"),
                   "A layer that shows nothing until it is painted on"},
                  paint_look_for_source(MA_PAINT_LAYER_SOURCE_IMAGE, PaintLookRole::Content)),
        PaintLayerAddKind::Paint);
    add(with_look(
            with_optional_color(
                {"FILL", N_("Fill Layer"), "A layer of one flat colour"}),
            paint_look_for_source(MA_PAINT_LAYER_SOURCE_CONSTANT, PaintLookRole::Content)),
        PaintLayerAddKind::Fill);
    add(with_look(
            {"FOLDER", N_("Folder"), "A group holding other layers, composited on its own"},
            paint_look_for_source(MA_PAINT_LAYER_SOURCE_STACK, PaintLookRole::Content)),
        PaintLayerAddKind::Folder);
    add(with_look(effect({"CORRECTION",
                          N_("Correction Paint"),
                          "A map painted over the active target: the layer's colour, or its mask "
                          "while a mask slot is the one showing"}),
                  paint_look_for_source(MA_PAINT_LAYER_SOURCE_IMAGE, PaintLookRole::Content)),
        PaintLayerAddKind::Correction,
        0,
        PaintLayerAddKind::MaskCorrection,
        N_("Paint"));
    add(with_look(effect(with_color({"CORRECTION_FILL",
                                     N_("Correction Fill"),
                                     "A flat colour laid over the active target: the layer's "
                                     "colour, or its mask while a mask slot is the one showing"})),
                  paint_look_for_source(MA_PAINT_LAYER_SOURCE_CONSTANT, PaintLookRole::Content)),
        PaintLayerAddKind::CorrectionFill,
        0,
        PaintLayerAddKind::MaskCorrectionFill,
        N_("Fill"));
    /* The mask paint correction keeps the built-in Grease Pencil icon its Add entry always had; the
     * Brush glyph is the look's, so the row it makes reads as the Paint it is. */
    add(with_look(effect({"MASK_CORRECTION",
                          N_("Mask Paint Correction"),
                          "A grey map painted over the active layer's mask, in Mask mode"}),
                  paint_look_for_source(MA_PAINT_LAYER_SOURCE_IMAGE, PaintLookRole::MaskItem)),
        PaintLayerAddKind::MaskCorrection);
    add(with_look(effect(with_color({"MASK_CORRECTION_FILL",
                                     N_("Mask Fill Correction"),
                                     "A flat grey laid over the active layer's mask"})),
                  paint_look_for_source(MA_PAINT_LAYER_SOURCE_CONSTANT, PaintLookRole::MaskItem)),
        PaintLayerAddKind::MaskCorrectionFill);

    /* Every filter as a correction of a layer's colour. */
    for (const PaintFilterAddKind &filter : paint_filter_add_kinds) {
      add(with_look(effect({filter.identifier, filter.name, filter.description}),
                    paint_look_for_source(
                        MA_PAINT_LAYER_SOURCE_FILTER, PaintLookRole::Content, filter.filter)),
          PaintLayerAddKind::Filter,
          filter.filter,
          PaintLayerAddKind::MaskFilter);
    }
    /* Then the same filters as rows of the stack: an adjustment changes the whole stack below it
     * instead of one layer's colour. */
    for (const PaintFilterAddKind &filter : paint_filter_add_kinds) {
      add(with_look({std::string("ADJUSTMENT") + (filter.identifier + std::strlen("FILTER")),
                     std::string(filter.name) + " Adjustment",
                     "Apply the filter to everything below this layer, with the layer's own opacity, "
                     "blend and mask"},
                    paint_look_for_source(
                        MA_PAINT_LAYER_SOURCE_FILTER, PaintLookRole::Content, filter.filter)),
          PaintLayerAddKind::Adjustment,
          filter.filter);
    }
    /* And once more in the mask: the filter changes the mask's coverage, a single grey, so it has
     * no channels to choose. They are corrections of the mask, listed with the other effects, and
     * carry the filter's own icon so a line reads the same whichever target is active. */
    for (const PaintFilterAddKind &filter : paint_filter_add_kinds) {
      add(with_look(effect({std::string("MASK_") + filter.identifier,
                            std::string("Mask ") + filter.name,
                            "Apply the filter to the active layer's mask coverage"}),
                    paint_look_for_source(
                        MA_PAINT_LAYER_SOURCE_FILTER, PaintLookRole::MaskItem, filter.filter)),
          PaintLayerAddKind::MaskFilter,
          filter.filter);
    }
    /* The map-driven filters of the mask. They have no content form, so the menu keeps them only
     * while a mask slot is the one showing. */
    for (const PaintFilterAddKind &filter : paint_coverage_add_kinds) {
      add(with_look(effect({std::string("MASK_") + filter.identifier,
                            std::string("Mask ") + filter.name,
                            filter.description}),
                    paint_look_for_source(
                        MA_PAINT_LAYER_SOURCE_FILTER, PaintLookRole::MaskItem, filter.filter)),
          PaintLayerAddKind::MaskFilter,
          filter.filter,
          std::nullopt,
          nullptr,
          true);
    }

    /* A layer that shows what another material shows, and the two places a correction can lay it:
     * the colour and the mask. */
    add(with_look(from_material({"MATERIAL",
                                 N_("Material Layer"),
                                 "A layer that shows what another material shows"},
                                "paint_layer_add_material"),
                  paint_look_for_source(MA_PAINT_LAYER_SOURCE_MATERIAL, PaintLookRole::Content)),
        PaintLayerAddKind::Material);
    add(with_look(effect(from_material(
                      {"CORRECTION_MATERIAL",
                       N_("Material Correction"),
                       "What another material shows, laid over the active target: the layer's "
                       "colour, or its mask while a mask slot is the one showing"},
                      "paint_layer_add_material_correction")),
                  paint_look_for_source(MA_PAINT_LAYER_SOURCE_MATERIAL, PaintLookRole::Content)),
        PaintLayerAddKind::CorrectionMaterial,
        0,
        PaintLayerAddKind::MaskCorrectionMaterial,
        N_("Material"));
    add(with_look(effect(from_material(
                      {"MASK_CORRECTION_MATERIAL",
                       N_("Mask Material Correction"),
                       "The grey of another material's Base Color, laid over the active layer's "
                       "mask"},
                      "paint_layer_add_material_mask_correction")),
                  paint_look_for_source(MA_PAINT_LAYER_SOURCE_MATERIAL, PaintLookRole::MaskItem)),
        PaintLayerAddKind::MaskCorrectionMaterial);

    /* Anchor Points: the marker and the three places a reader can land. A marker picks its list
     * from the active row; a reader's item decides the list, the way the three Material items do.
     */
    add(with_look(effect({"ANCHOR_POINT",
                          N_("Anchor Point"),
                          "Publish the active layer's content in its Effects up to this place; "
                          "select a mask item first to put the point in the mask instead"}),
                  paint_look_for_anchor_point()),
        PaintLayerAddKind::AnchorPoint);
    add(with_look({"ANCHOR_POINT_LAYER",
                   N_("Anchor Point Layer"),
                   "A layer that shows the slice an Anchor Point of this material publishes"},
                  paint_look_for_anchor_reader()),
        PaintLayerAddKind::AnchorPointLayer);
    add(with_look(effect({"ANCHOR_POINT_CORRECTION",
                          N_("Anchor Point Correction"),
                          "The slice an Anchor Point publishes, laid over the active target: the "
                          "layer's colour, or its mask while a mask slot is the one showing"}),
                  paint_look_for_anchor_reader()),
        PaintLayerAddKind::AnchorPointCorrection,
        0,
        PaintLayerAddKind::AnchorPointMaskCorrection,
        N_("Anchor Point Reader"));
    add(with_look(effect({"ANCHOR_POINT_MASK_CORRECTION",
                          N_("Anchor Point Mask Correction"),
                          "The slice an Anchor Point publishes, laid over the active layer's mask"}),
                  paint_look_for_anchor_reader()),
        PaintLayerAddKind::AnchorPointMaskCorrection);

    /* A procedural ramp built from Shader Editor nodes: as a layer, and in the two correction
     * roles. The one Gradient glyph marks the ramp wherever it appears. */
    add(with_look({"GRADIENT",
                   N_("Gradient Layer"),
                   "A procedural colour and coverage ramp reading the UV, built from shader nodes"},
                  paint_look_for_source(MA_PAINT_LAYER_SOURCE_GENERATOR, PaintLookRole::Content)),
        PaintLayerAddKind::Gradient);
    add(with_look(effect({"CORRECTION_GRADIENT",
                          N_("Gradient Correction"),
                          "A procedural ramp laid over the active target: the layer's colour, or "
                          "its mask while a mask slot is the one showing"}),
                  paint_look_for_source(MA_PAINT_LAYER_SOURCE_GENERATOR, PaintLookRole::Content)),
        PaintLayerAddKind::GradientCorrection,
        0,
        PaintLayerAddKind::MaskGradient,
        N_("Gradient"));
    add(with_look(effect({"MASK_CORRECTION_GRADIENT",
                          N_("Mask Gradient Correction"),
                          "A procedural ramp's grey laid over the active layer's mask"}),
                  paint_look_for_source(MA_PAINT_LAYER_SOURCE_GENERATOR, PaintLookRole::MaskItem)),
        PaintLayerAddKind::MaskGradient);

    /* 3D control curves stamped through a brush snapshot of the effect's own. Only an effect: there
     * is no Curve Patch layer. */
    add(with_look(effect({"CURVE_PATCH_CORRECTION",
                          N_("Curve Patch"),
                          "3D control curves stamped through the effect's own brush snapshot, laid "
                          "over the active layer's colour, below the layer's own blend"}),
                  paint_look_for_source(MA_PAINT_LAYER_SOURCE_CURVE_PATCH, PaintLookRole::Content)),
        PaintLayerAddKind::CurvePatchEffect);

    /* Resolve the mask roles the content entries named. The mask entry is flagged so the Effects
     * menu folds it into the content line; the content entry keeps its place in the table. */
    for (const int64_t twin : twin_rows.index_range()) {
      const PaintAddKindEntry &content = table[twin_rows[twin]];
      for (const int64_t index : table.index_range()) {
        if (table[index].kind == *twin_kinds[twin] && table[index].filter == content.filter) {
          table[twin_rows[twin]].mask_twin = int(index);
          table[index].is_mask_twin = true;
          break;
        }
      }
    }
    return table;
  }();
  return entries.as_span();
}

/**
 * What the Effects menu's corrections lay over right now: the row's mask while a mask slot is the
 * one showing, its colour otherwise. This is the question the folded entries answer.
 */
static bool paint_effect_menu_target_is_mask(bContext &C)
{
  const Scene *scene = CTX_data_scene(&C);
  return scene != nullptr && scene->toolsettings != nullptr &&
         scene->toolsettings->paint_mode.layer_target_mode == PAINT_LAYER_TARGET_MASK;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add UI
 *
 * What the paint source adds to the generic Add dialog: the channels an empty Fill starts with,
 * and the palette a Fill colour can be picked from. The dialog itself, its kinds and its colour
 * belong to the Outliner; only the paint vocabulary lives here.
 * \{ */

/** The layered material this Add writes to, or null when the space has no material owner. */
static Material *paint_add_material_owner(bContext &C)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return nullptr;
  }
  ID *owner = outliner_stack_owner_get(outliner_stack_read_context(C), *space_outliner);
  /* The source's own owner resolution: for this source it is the layered material, so the type is
   * known here and the generic layer never needs the material's name prefix. */
  return owner != nullptr ? id_cast<Material *>(owner) : nullptr;
}

/** The channel a flag item's bit names; -1 for the sentinel or a value that is not one bit. */
static int paint_add_channel_from_item(const EnumPropertyItem &item)
{
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if ((1 << channel) == item.value) {
      return channel;
    }
  }
  return -1;
}

/**
 * The channel records an empty Fill may start with: the channels of the material's set, as the
 * same short toggle buttons the layer widgets use. None is selected by default; the row stays
 * inert until the user picks the channels its image set needs.
 */
static void paint_add_empty_channels_draw(bContext &C, wmOperator &op, ui::Layout &layout)
{
  Material *material = paint_add_material_owner(C);
  PropertyRNA *channels_prop = RNA_struct_find_property(op.ptr, "fill_channels");
  if (material == nullptr || channels_prop == nullptr) {
    return;
  }
  layout.label(IFACE_("Channels to Start With"), ICON_NONE);
  ui::Layout &col = layout.column(false);
  /* The shared table is declared with an open bound, so it cannot be range-for'd; walk to its
   * sentinel instead. */
  for (const EnumPropertyItem *item = rna_enum_material_paint_channel_flag_items;
       item->identifier != nullptr;
       item++)
  {
    const int channel = paint_add_channel_from_item(*item);
    if (channel < 0 ||
        !BKE_paint_layers_channel_in_set(*material, eMaterialPaintChannel(channel)))
    {
      continue;
    }
    /* The same flag checkbox the Active Channel panel draws. */
    col.row(true).prop_enum(op.ptr, channels_prop, item->identifier, std::nullopt, ICON_NONE);
  }
}

/**
 * The palette of the active paint mode, with its selector, writing a swatch click into the Add's
 * own `fill_color`: a Fill is picked from the palette the user already paints with. Nothing is
 * drawn outside a paint context, where there is no palette to show.
 */
static void paint_add_palette_draw(bContext &C, wmOperator &op, ui::Layout &layout)
{
  Paint *paint = BKE_paint_get_active_from_context(&C);
  Scene *scene = CTX_data_scene(&C);
  if (paint == nullptr || scene == nullptr) {
    return;
  }
  PointerRNA paint_ptr = RNA_pointer_create_discrete(
      &scene->id, RNA_struct_find("Paint"), paint);
  layout.separator();
  layout.label(IFACE_("Color Palette"), ICON_COLOR);
  /* The same selector the standard color picker's palette panel shows, so a stack with no
   * palette assigned can still pick or create one. */
  ui::template_id(&layout, &C, &paint_ptr, "palette", "palette.new", nullptr, nullptr);
  if (BKE_paint_palette(paint) != nullptr) {
    ui::template_palette(&layout, &paint_ptr, "palette", true, false, op.ptr, "fill_color");
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paint-specific operators
 *
 * Operators whose subject is a paint layer rather than a row: entering a Curve Patch effect's
 * tool, and handing a row to the paint operator that owns brush selection. They live here, with
 * the rest of the paint vocabulary, so the generic operator file names no material.
 * \{ */

/** The active row must be a Curve Patch effect with a payload, for the tool to enter. The caller
 * activates the row first when the click aimed at another one (see the double-click handler). */
static bool stack_layer_curve_patch_edit_poll(bContext *C)
{
  if (!stack_mode_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr || GS(owner->name) != ID_MA) {
    return false;
  }
  Material &material = id_cast<Material &>(*owner);
  const MaterialPaintLayer *row = BKE_paint_layers_active_layer_get(material);
  return row != nullptr && row->source == MA_PAINT_LAYER_SOURCE_CURVE_PATCH &&
         row->curve_patch != nullptr;
}

static wmOperatorStatus stack_layer_curve_patch_edit_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  const StackRow *row = (owner != nullptr) ? outliner_stack_row_find(*space_outliner, ordinal) :
                                             nullptr;
  if (owner == nullptr || row == nullptr) {
    return OPERATOR_CANCELLED;
  }
  Material &material = id_cast<Material &>(*owner);
  MaterialPaintLayer *layer = BKE_paint_layers_find(material, row->stable_id);
  if (layer == nullptr || layer->source != MA_PAINT_LAYER_SOURCE_CURVE_PATCH ||
      layer->curve_patch == nullptr)
  {
    BKE_report(op->reports, RPT_WARNING, "The selected row is not a Curve Patch effect");
    return OPERATOR_CANCELLED;
  }
  /* The tool runs on the active row: activate it when the edit was aimed elsewhere. A refused
   * switch (the live-session confirm) must not start a second session on top. */
  if (!BLI_uuid_equal(material.active_layer_marker, row->stable_id)) {
    if (!outliner_stack_row_activate(C, *space_outliner, ordinal)) {
      return OPERATOR_CANCELLED;
    }
  }
  if (!blender::ed::sculpt_paint::curve_patch_layer_activate(*C, material, *layer)) {
    return OPERATOR_CANCELLED;
  }
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

/** Double-click on a Curve Patch effect row: the explicit entrance into the row's curve tool. */
void OUTLINER_OT_stack_layer_curve_patch_edit(wmOperatorType *ot)
{
  ot->name = "Edit Curve Patch Effect";
  ot->idname = "OUTLINER_OT_stack_layer_curve_patch_edit";
  ot->description =
      "Enter the Curve Patch tool of the layer: its brush snapshot becomes the active brush and "
      "its curves open in the modal editor";
  ot->exec = stack_layer_curve_patch_edit_exec;
  ot->poll = stack_layer_curve_patch_edit_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  rna_def_stack_row_ordinal(ot, "Curve Patch effect to edit; -1 uses the active one");
  rna_def_stack_row_marker(ot);
}

/**
 * "Use as Brush Source": hand the row under the menu to the paint operator that owns brush
 * selection. The outliner knows the material and the row's stable marker; it does not touch the
 * brush itself.
 */
static wmOperatorStatus stack_row_use_as_brush_source_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  const StackRow *row = outliner_stack_row_find(*space_outliner, ordinal);
  if (owner == nullptr || row == nullptr || BLI_uuid_is_nil(row->stable_id)) {
    return OPERATOR_CANCELLED;
  }
  if (GS(owner->name) != ID_MA) {
    BKE_report(op->reports, RPT_ERROR, "The stack row is not part of a material");
    return OPERATOR_CANCELLED;
  }

  wmOperatorType *ot = WM_operatortype_find("PAINT_OT_brush_layer_source_assign_from_stack",
                                            false);
  if (ot == nullptr) {
    return OPERATOR_CANCELLED;
  }
  PointerRNA props = WM_operator_properties_create_ptr(ot);
  WM_operator_properties_id_lookup_set_from_id(&props, owner);
  char marker_str[UUID_STRING_SIZE];
  BLI_uuid_format(marker_str, row->stable_id);
  RNA_string_set(&props, "marker", marker_str);
  const wmOperatorStatus ret = WM_operator_name_call_ptr(
      C, ot, wm::OpCallContext::ExecDefault, &props, nullptr);
  WM_operator_properties_free(&props);
  return ret;
}

static bool stack_row_use_as_brush_source_poll(bContext *C)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr || GS(owner->name) != ID_MA ||
      !BKE_paint_layers_is_layered(*reinterpret_cast<Material *>(owner)))
  {
    return false;
  }
  Paint *paint = BKE_paint_get_active_from_context(C);
  return paint != nullptr && BKE_paint_brush(paint) != nullptr;
}

void OUTLINER_OT_stack_layer_use_as_brush_source(wmOperatorType *ot)
{
  ot->name = "Use as Brush Source";
  ot->idname = "OUTLINER_OT_stack_layer_use_as_brush_source";
  ot->description = "Use the stack row as the active PBR Paint brush's Layer source";
  ot->exec = stack_row_use_as_brush_source_exec;
  ot->poll = stack_row_use_as_brush_source_poll;
  /* No undo of its own: the paint operator owns the brush/preset change and its undo push. */
  ot->flag = OPTYPE_INTERNAL;

  rna_def_stack_row_ordinal(ot, "Row to use as the source; -1 uses the active one");
  rna_def_stack_row_marker(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The source
 * \{ */

class PaintLayersStackSource final : public StackSource,
                                     public StackEditor,
                                     public StackGroupingEditor {
 public:
  eSpaceOutliner_StackSource type() const override
  {
    return SO_STACK_SRC_PAINT_MATERIAL;
  }

  StringRefNull ui_name() const override
  {
    return N_("Paint Layers");
  }

  bool object_has_stack(const StackReadContext & /*ctx*/, Object &object) const override
  {
    const Material *material = BKE_object_material_get(&object, object.actcol);
    return material != nullptr && BKE_paint_layers_is_layered(*material);
  }

  ID *object_preview_id(const StackReadContext & /*ctx*/, Object &object) const override
  {
    Material *material = BKE_object_material_get(&object, object.actcol);
    return (material != nullptr) ? &material->id : nullptr;
  }

  ID *owner_get(const StackReadContext &ctx, const StackFocus &focus) const override
  {
    Object *object = outliner_stack_focus_object_get(ctx, focus);
    if (object == nullptr) {
      return nullptr;
    }
    const short slot = focus.sub_index >= 0 ? short(focus.sub_index + 1) : object->actcol;
    Material *material = BKE_object_material_get(object, slot);
    return (material != nullptr) ? &material->id : nullptr;
  }

  uint64_t state_hash(const StackReadContext &ctx, const ID &owner) const override
  {
    const Material &material = layers_owner(owner);
    uint64_t hash = uint64_t(material.paint_layers_flag);
    /* The header's channel cursor changes which pair the columns address, so the rows rebuild
     * when it moves. */
    hash = hash * 1000003u ^ uint64_t(paint_stack_selected_channel(ctx, material));
    /* The set decides the swatches and the cursor's stand-in, and an edit of it moves no marker. */
    hash = hash * 1000003u ^ uint64_t(BKE_paint_layers_channel_set_mask_get(material));
    /* The action button reads pressed from this flag, so the rows have to rebuild when it flips. */
    if (ctx.scene != nullptr && ctx.scene->toolsettings != nullptr) {
      hash = hash * 1000003u ^
             uint64_t(ctx.scene->toolsettings->paint_mode.layer_object_mask_edit != 0);
    }
    BKE_paint_layers_foreach(
        material,
        [&](const MaterialPaintLayer &layer, const MaterialPaintLayer *parent) -> bool {
      /* The nesting is part of the rows: moving a row into a folder changes no marker. */
      hash = hash * 1000003u ^ (parent != nullptr ? UUID(parent->marker).hash() : 0);
      hash = hash * 1000003u ^ UUID(layer.marker).hash();
      hash ^= uint64_t(layer.flag);
      /* Mixed, not shifted into the flag bits: `flag` is 16 bits wide and would overlap. */
      hash = hash * 1000003u ^ uint64_t(layer.blend);
      hash ^= uint64_t(layer.source) << 16;
      /* The group's folder icon and hierarchy line are drawn from the tag, so a change to it has
       * to reach the rows even though nothing evaluated reads it. */
      hash = hash * 1000003u ^ uint64_t(uint8_t(layer.color_tag));
      /* The swatch and the columns show these, so a change to them has to reach the rows. */
      hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(layer.opacity));
      for (const MaterialPaintLayerChannelSettings &settings : layer.channel_settings) {
        hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(settings.opacity));
        hash = hash * 1000003u ^ uint64_t(uint8_t(settings.blend));
      }
      for (const char *ch = layer.name; *ch != '\0'; ch++) {
        hash = hash * 131u ^ uint64_t(uint8_t(*ch));
      }
      /* The mask slot shows the mask's map, or its constant before it has one. */
      if (const MaterialPaintLayer *mask = BKE_paint_layers_mask_base(layer)) {
        hash = hash * 1000003u ^ uint64_t(mask->flag);
        if (const MaterialPaintLayerChannel *record = paint_mask_record(*mask)) {
          hash = hash * 1000003u ^
                 uint64_t(std::hash<float>{}(BKE_paint_layers_mask_constant(*mask)));
          if (record->image != nullptr) {
            hash ^= uint64_t(record->image->id.session_uid) * 2654435761u;
            hash = hash * 1000003u ^ uint64_t(paint_image_is_blank(*record->image) ? 1 : 0);
          }
        }
      }
      for (int c = 0; c < layer.channels_num; c++) {
        const Image *image = layer.channels[c].image;
        hash ^= uint64_t(layer.channels[c].channel) << 32;
        hash ^= uint64_t(layer.channels[c].state) << 40;
        if (image != nullptr) {
          hash ^= uint64_t(image->id.session_uid) * 2654435761u;
          /* A blank generated map is drawn as a placeholder; the first stroke that touches it
           * flips this, so the rows (and their previews) must rebuild. */
          hash = hash * 1000003u ^ uint64_t(paint_image_is_blank(*image) ? 1 : 0);
        }
      }
      /* A mask correction is a row of its own too: its blend, opacity, name and grey or map. */
      for (const MaterialPaintLayer &item : layer.mask_stack) {
        if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
          continue;
        }
        hash = hash * 1000003u ^ UUID(item.marker).hash();
        hash ^= uint64_t(item.flag);
        hash = hash * 1000003u ^ uint64_t(item.blend);
        hash ^= uint64_t(item.source) << 16;
        hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(item.opacity));
        for (const char *ch = item.name; *ch != '\0'; ch++) {
          hash = hash * 131u ^ uint64_t(uint8_t(*ch));
        }
        if (const MaterialPaintLayerChannel *record = paint_mask_record(item)) {
          hash = hash * 1000003u ^
                 uint64_t(std::hash<float>{}(BKE_paint_layers_mask_constant(item)));
          if (record->image != nullptr) {
            hash ^= uint64_t(record->image->id.session_uid) * 2654435761u;
            hash = hash * 1000003u ^ uint64_t(paint_image_is_blank(*record->image) ? 1 : 0);
          }
        }
      }
      /* The corrections are rows of their own: what their rows show has to reach the tree too. */
      for (const MaterialPaintLayer &effect : layer.effects) {
        hash = hash * 1000003u ^ UUID(effect.marker).hash();
        hash ^= uint64_t(effect.flag);
        hash = hash * 1000003u ^ uint64_t(effect.blend);
        hash ^= uint64_t(effect.source) << 16;
        hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(effect.opacity));
        for (const MaterialPaintLayerChannelSettings &settings : effect.channel_settings) {
          hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(settings.opacity));
          hash = hash * 1000003u ^ uint64_t(uint8_t(settings.blend));
        }
        for (const char *ch = effect.name; *ch != '\0'; ch++) {
          hash = hash * 131u ^ uint64_t(uint8_t(*ch));
        }
        for (int c = 0; c < effect.channels_num; c++) {
          const Image *image = effect.channels[c].image;
          hash ^= uint64_t(effect.channels[c].channel) << 32;
          hash ^= uint64_t(effect.channels[c].state) << 40;
          if (image != nullptr) {
            hash ^= uint64_t(image->id.session_uid) * 2654435761u;
            hash = hash * 1000003u ^ uint64_t(paint_image_is_blank(*image) ? 1 : 0);
          }
          else if (effect.source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
            /* The swatch shows the colour, so an edit of it has to reach the row. */
            for (const int k : IndexRange(4)) {
              hash = hash * 1000003u ^ uint64_t(std::hash<float>{}(effect.channels[c].value[k]));
            }
          }
        }
      }
      return true;
    });
    return hash;
  }

  bool rows_build(const StackReadContext &ctx,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  Vector<StackRow> &r_rows) const override
  {
    const Material &material = layers_owner(owner);
    paint_stack_rows_from_description(
        material, paint_stack_selected_channel(ctx, material), r_rows);
    if (ctx.scene != nullptr && ctx.scene->toolsettings != nullptr &&
        ctx.scene->toolsettings->paint_mode.layer_object_mask_edit != 0)
    {
      for (StackRow &row : r_rows) {
        row.action_active = row.action_icon != ICON_NONE && !BLI_uuid_is_nil(row.stable_id) &&
                            BLI_uuid_equal(material.active_layer_marker, row.stable_id);
      }
    }
    return !r_rows.is_empty();
  }

  bool is_editable(const ID &owner) const override
  {
    const Material &material = layers_owner(owner);
    return ID_IS_EDITABLE(&material.id) && !ID_IS_OVERRIDE_LIBRARY(&material.id);
  }

  StackColumnLayout column_layout() const override
  {
    StackColumnLayout layout;
    layout.value_width = 2.6f;
    /* Wide enough for the compact "NN% Abr" label; the blend column shares the width. */
    layout.mode_width = 2.6f;
    layout.action_width = 1.0f;
    return layout;
  }

  bool supports_row_lock() const override
  {
    return true;
  }

  bool row_action(bContext &C,
                  ID &owner,
                  const int ordinal,
                  const StackRow &row) const override
  {
    Scene *scene = CTX_data_scene(&C);
    SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
    if (scene == nullptr || scene->toolsettings == nullptr || space_outliner == nullptr) {
      return false;
    }
    /* The action button of a Curve Patch effect enters its tool: activate the effect, then open
     * the session (which switches to the effect's brush snapshot and blocks the other tools). */
    {
      Material &material = layers_owner(owner);
      if (MaterialPaintLayer *effect = BKE_paint_layers_find(material, row.stable_id);
          effect != nullptr && effect->source == MA_PAINT_LAYER_SOURCE_CURVE_PATCH)
      {
        /* The activation also raises the live-session confirm; do not start a second session over
         * it when the switch was refused. */
        if (!outliner_stack_row_activate(&C, *space_outliner, ordinal)) {
          return false;
        }
        return blender::ed::sculpt_paint::curve_patch_layer_activate(C, material, *effect);
      }
    }
    PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
    if (row.action_active) {
      paint_mode.layer_object_mask_edit = 0;
    }
    else {
      /* Activating may switch the target mode, which clears the flag -- so set it after. The
       * activation changes data (the active row), so it gets an undo step like the activate
       * operator does. */
      const Material &material = layers_owner(owner);
      const bool active_changed = !BLI_uuid_equal(material.active_layer_marker, row.stable_id);
      outliner_stack_row_activate(&C, *space_outliner, ordinal);
      paint_mode.layer_object_mask_edit = 1;
      /* Scan the UVs once now, so the Properties redraws only read the tile cache. */
      BKE_paint_layer_object_mask_tiles_prefetch(*CTX_data_main(&C));
      if (active_changed) {
        ED_undo_push(&C, "Activate Paint Layer");
      }
    }
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
    return true;
  }

  bool row_activate(bContext &C,
                    const StackFocus & /*focus*/,
                    ID &owner,
                    const int /*ordinal*/,
                    const StackRow &row) const override
  {
    Material &material = layers_owner(owner);
    if (!this->is_editable(owner)) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Layered material is not editable");
      return false;
    }
    if (!row.supported) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Unsupported layer cannot be activated");
      return false;
    }
    return sculpt_paint::paint_stack_row_activate(C, material, row.stable_id);
  }

  bool preview_activate(bContext &C,
                        ID & /*owner*/,
                        const StackRow &row,
                        const StringRef section_id) const override
  {
    bool has_section = false;
    for (const StackContentSection &section : row.content_sections) {
      has_section |= section.identifier == section_id;
    }
    if (!has_section) {
      return false;
    }
    /* A click on a slot switches what the brush paints: the mask, or the content. */
    if (section_id == "MASK") {
      return sculpt_paint::paint_stack_target_mode_set(C, PAINT_LAYER_TARGET_MASK);
    }
    if (section_id == "CHANNELS") {
      return sculpt_paint::paint_stack_target_mode_set(C, PAINT_LAYER_TARGET_CONTENT);
    }
    return false;
  }

  bool row_is_active(const StackReadContext & /*ctx*/,
                     const StackFocus & /*focus*/,
                     const ID &owner,
                     const StackRow &row) const override
  {
    const Material &material = layers_owner(owner);
    return !BLI_uuid_is_nil(row.stable_id) &&
           BLI_uuid_equal(material.active_layer_marker, row.stable_id);
  }

  void sub_selections_get(const StackReadContext &ctx,
                          const StackFocus &focus,
                          Vector<StackSubSelection> &r_items) const override
  {
    Object *object = outliner_stack_focus_object_get(ctx, focus);
    if (object == nullptr) {
      return;
    }
    /* One entry per material slot: the stack the Outliner shows is one material's, and an object
     * can carry several. */
    for (const int slot : IndexRange(object->totcol)) {
      const Material *material = BKE_object_material_get(object, short(slot + 1));
      StackSubSelection item;
      item.name = material != nullptr ? material->id.name + 2 : "";
      item.preview_id = (material != nullptr) ? &const_cast<Material *>(material)->id : nullptr;
      r_items.append(std::move(item));
    }
  }

  int default_sub_index(const StackReadContext &ctx, const StackFocus &focus) const override
  {
    Object *object = outliner_stack_focus_object_get(ctx, focus);
    return (object != nullptr && object->actcol > 0) ? int(object->actcol) - 1 : -1;
  }

  void sub_selection_apply(bContext & /*C*/,
                           const StackFocus &focus,
                           Object &object) const override
  {
    /* Choosing a stack slot is choosing the object's active material slot. */
    if (focus.sub_index >= 0 && focus.sub_index < object.totcol) {
      object.actcol = short(focus.sub_index + 1);
      /* As `Object.active_material_index` does: an edit-mesh assigns new faces from its own copy
       * of the slot. */
      if (object.type == OB_MESH) {
        Mesh *mesh = id_cast<Mesh *>(object.data);
        if (mesh->runtime->edit_mesh) {
          mesh->runtime->edit_mesh->mat_nr = focus.sub_index;
        }
      }
    }
  }

  void add_kind_channel_items(bContext &C, Vector<EnumPropertyItem> &r_items) const override
  {
    const Material *material = paint_add_material_owner(C);
    if (material == nullptr) {
      return;
    }
    /* The shared table has an open bound; walk it to its sentinel. */
    for (const EnumPropertyItem *item = rna_enum_material_paint_channel_flag_items;
         item->identifier != nullptr;
         item++)
    {
      const int channel = paint_add_channel_from_item(*item);
      if (channel < 0 ||
          !BKE_paint_layers_channel_in_set(*material, eMaterialPaintChannel(channel)))
      {
        continue;
      }
      r_items.append(*item);
    }
  }

  void add_kind_draw_extra(bContext &C,
                           const StackAddKindInfo &kind,
                           wmOperator &op,
                           ui::Layout &layout,
                           const bool empty) const override
  {
    if (!kind.color_optional) {
      return;
    }
    if (empty) {
      paint_add_empty_channels_draw(C, op, layout);
    }
    else {
      paint_add_palette_draw(C, op, layout);
    }
  }

  const StackEditor *editor() const override
  {
    return this;
  }

  /**
   * Stop aiming strokes at the row the stack was activated to. The target is
   * #Material::active_layer_marker, so dropping the marker is the whole edit; the mask-edit cursor
   * goes back to content because a mask belonging to a row that is no longer the target is not
   * something a stroke should keep writing into.
   */
  bool target_clear(bContext &C) const override
  {
    /* The owner the Outliner is focused on, not the active object: the operator acts on the stack
     * being shown. */
    SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
    if (space_outliner == nullptr) {
      return false;
    }
    ID *owner = outliner_stack_owner_get(outliner_stack_read_context(C), *space_outliner);
    if (owner == nullptr || GS(owner->name) != ID_MA) {
      return false;
    }
    Material &material = layers_owner(*owner);
    if (!paint_layers_edit_target_clear(material)) {
      return false;
    }
    /* Back to the neutral target: #row_activate may have put the mode in Mask, and an armed
     * object-mask edit belongs to the row that is no longer the target. This also ends Mask mode
     * and restores the content brush when it does. */
    sculpt_paint::paint_stack_target_mode_set(C, PAINT_LAYER_TARGET_CONTENT);
    /* Even when the mode already was Content, the slots held the dropped row's maps. */
    sculpt_paint::paint_stack_texpaint_slots_refresh(C);
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  std::optional<eObjectMode> focus_object_mode() const override
  {
    return OB_MODE_SCULPT;
  }

  const char *focus_tool_idname() const override
  {
    return "builtin_brush.paint";
  }

  const StackDropHandler *drop_handler() const override
  {
    return drop_handler_.get();
  }

  StringRefNull default_section_id() const override
  {
    return "CHANNELS";
  }

  bool notifier_invalidates(const wmNotifier &notifier) const override
  {
    if (notifier.category == NC_MATERIAL) {
      return true;
    }
    /* The Stack Layers channel selection changes which pair the columns address. */
    if (notifier.category == NC_SPACE && notifier.data == ND_SPACE_OUTLINER) {
      return true;
    }
    /* A first stroke materializes an image the rows list. */
    if (notifier.category == NC_IMAGE) {
      return ELEM(notifier.action, NA_ADDED, NA_REMOVED, NA_EDITED);
    }
    return false;
  }

  /* ---------------------------------------------------------------- */
  /** \name StackEditor
   * \{ */

  bool can_reorder(const ID &owner) const override
  {
    return this->is_editable(owner);
  }

  bool row_reorder(bContext &C,
                   const StackFocus & /*focus*/,
                   ID &owner,
                   const int from_ordinal,
                   const int to_ordinal) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *from = paint_description_row_for_ordinal(material, from_ordinal);
    MaterialPaintLayer *to = paint_description_row_for_ordinal(material, to_ordinal);
    if (from == nullptr || to == nullptr) {
      return false;
    }
    if (!BKE_paint_layers_edit_reorder(material, *from, *to)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  void add_kinds(Vector<StackAddKindInfo> &r_kinds) const override
  {
    /* #row_add reads a kind back from the same table by its index. */
    for (const PaintAddKindEntry &entry : paint_add_kind_entries()) {
      r_kinds.append(entry.info);
    }
  }

  /**
   * One line per thing: the corrections that exist over the colour and over the mask are listed
   * once, and the line resolves to the role the active slot asks for. The emitted #kind is already
   * that role's entry, so #row_add stays a plain "create the kind you were given" verb and a
   * script that names a kind is never reinterpreted by the current target mode.
   */
  void effect_menu_kinds(bContext &C, Vector<StackEffectMenuKind> &r_kinds) const override
  {
    const Span<PaintAddKindEntry> entries = paint_add_kind_entries();
    const bool target_is_mask = paint_effect_menu_target_is_mask(C);
    for (const int64_t index : entries.index_range()) {
      const PaintAddKindEntry &entry = entries[index];
      if (!entry.info.is_effect || entry.is_mask_twin) {
        continue;
      }
      /* A mask-only kind has no content form, so it only makes sense while a mask slot is the one
       * showing. */
      if (!target_is_mask && entry.mask_only) {
        continue;
      }
      StackEffectMenuKind menu;
      menu.kind = int(index);
      menu.info = entry.info;
      if (entry.menu_name != nullptr) {
        /* Untranslated, like every name in the table: the itemf translates it. */
        menu.info.name = entry.menu_name;
      }
      if (target_is_mask && entry.mask_twin >= 0) {
        /* A mask slot is the one showing: create the mask role of the line, and say what it lays
         * there with the icon that says mask. The short name stays, so the menu reads the same
         * whichever slot is active. */
        const PaintAddKindEntry &twin = entries[entry.mask_twin];
        menu.kind = entry.mask_twin;
        menu.info.identifier = twin.info.identifier;
        menu.info.description = twin.info.description;
        menu.info.icon = twin.info.icon;
        menu.info.glyph = twin.info.glyph;
      }
      r_kinds.append(std::move(menu));
    }
  }

  int row_add(bContext &C,
              const StackFocus & /*focus*/,
              ID &owner,
              const int kind,
              const int ordinal,
              const StackAddArgs &args = {}) const override
  {
    const Span<PaintAddKindEntry> entries = paint_add_kind_entries();
    if (kind < 0 || kind >= entries.size() || !this->is_editable(owner)) {
      return -1;
    }
    const PaintAddKindEntry &entry = entries[kind];
    /* The kind is created as given: the Effects menu already resolved the role the active slot
     * asks for, so a script that names a kind is not reinterpreted by the current target mode. */
    const PaintLayerAddKind add_kind = entry.kind;
    const bool is_anchor_point = add_kind == PaintLayerAddKind::AnchorPoint;
    const bool is_anchor_point_layer = add_kind == PaintLayerAddKind::AnchorPointLayer;
    const bool is_anchor_point_correction = add_kind == PaintLayerAddKind::AnchorPointCorrection;
    const bool is_anchor_point_mask_correction = add_kind ==
                                                 PaintLayerAddKind::AnchorPointMaskCorrection;
    const bool is_anchor_point_reader = is_anchor_point_layer || is_anchor_point_correction ||
                                        is_anchor_point_mask_correction;
    const bool is_material = add_kind == PaintLayerAddKind::Material;
    const bool reads_material = ELEM(add_kind,
                                     PaintLayerAddKind::Material,
                                     PaintLayerAddKind::CorrectionMaterial,
                                     PaintLayerAddKind::MaskCorrectionMaterial);
    const bool is_adjustment = add_kind == PaintLayerAddKind::Adjustment;
    /* A filter correction hangs on a layer; an adjustment is a layer. */
    const bool is_filter = add_kind == PaintLayerAddKind::Filter;
    Material &material = layers_owner(owner);
    MaterialPaintLayer *anchor = (ordinal < 0) ?
                                     nullptr :
                                     paint_description_row_for_ordinal(material, ordinal);
    if (ordinal >= 0 && anchor == nullptr) {
      if (is_anchor_point || is_anchor_point_reader) {
        BKE_report(CTX_wm_reports(&C),
                   RPT_WARNING,
                   "The selected row is no longer part of this material");
      }
      return -1;
    }
    PaintLayerAddParams params;
    params.filter = entry.filter;
    if (reads_material) {
      /* The Add resolved the data-block it names before the call; a kind that needs one is never
       * called without it, but the call is also open to scripts. */
      if (args.source == nullptr || GS(args.source->name) != ID_MA) {
        BKE_report(CTX_wm_reports(&C), RPT_WARNING, "A Material layer needs a material to read");
        return -1;
      }
      params.source = id_cast<Material *>(args.source);
      /* Each Material row reads a material of its own, so its settings are its own. */
      params.bmain = CTX_data_main(&C);
    }
    params.kind = add_kind;
    if (params.kind == PaintLayerAddKind::CurvePatchEffect) {
      /* The payload is created with the effect: a database for its two IDs, and the active brush as
       * the snapshot source (a fresh default stands in when none is active). */
      params.bmain = CTX_data_main(&C);
      const Scene *scene = CTX_data_scene(&C);
      if (scene != nullptr && scene->toolsettings != nullptr &&
          scene->toolsettings->sculpt != nullptr)
      {
        params.curve_patch_brush = BKE_paint_brush(&scene->toolsettings->sculpt->paint);
      }
    }
    if (is_anchor_point || is_anchor_point_reader) {
      /* The active item decides where the point lands: a layer or a content correction publishes
       * content, a mask item publishes the mask. Canonicalize the effect/mask item to its owner
       * row, so a layer inserted beside the active thing is always a stack row. */
      MaterialPaintLayer *active_item = nullptr;
      MaterialPaintLayer *active_effect = nullptr;
      MaterialPaintLayer *active_row = nullptr;
      if (ordinal >= 0) {
        if (anchor->role == MA_PAINT_LAYER_ROLE_EFFECT) {
          active_effect = anchor;
          active_row = BKE_paint_layers_correction_owner(material, *anchor);
        }
        else if (anchor->role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
          active_item = anchor;
          active_row = BKE_paint_layers_mask_item_owner(material, *anchor);
        }
        else {
          active_row = anchor;
        }
      }
      else {
        active_item = BKE_paint_layers_active_mask_item_get(material);
        MaterialPaintLayer *active = BKE_paint_layers_active_layer_get(material);
        if (active != nullptr && active->role == MA_PAINT_LAYER_ROLE_EFFECT) {
          active_effect = active;
          active_row = BKE_paint_layers_correction_owner(material, *active);
        }
        else {
          active_row = active;
        }
      }
      if (active_item == nullptr && active_effect == nullptr && active_row != nullptr) {
        /* The mask's base is no Outliner row: selecting its slot leaves the layer active and only
         * switches the global target to the mask. That is the user pointing at the mask, so the
         * point goes there, right after the base. */
        const Scene *scene = CTX_data_scene(&C);
        if (scene != nullptr && scene->toolsettings != nullptr &&
            scene->toolsettings->paint_mode.layer_target_mode == PAINT_LAYER_TARGET_MASK)
        {
          active_item = BKE_paint_layers_mask_base(*active_row);
        }
      }
      MaterialPaintLayer *point_owner = active_row;
      MaterialPaintLayer *insert_after = nullptr;
      PaintLayerAnchorPointList list = PaintLayerAnchorPointList::Effects;
      if (is_anchor_point_layer) {
        /* A reader layer is a row like a Material layer: above the active row, or on top. */
        list = PaintLayerAnchorPointList::Row;
        insert_after = active_row;
      }
      else if (is_anchor_point_mask_correction) {
        list = PaintLayerAnchorPointList::MaskStack;
        insert_after = active_item;
      }
      else if (is_anchor_point_correction) {
        list = PaintLayerAnchorPointList::Effects;
        insert_after = active_effect;
      }
      else {
        /* Marker: content under the active layer/effect, mask under the active mask item. */
        if (active_item != nullptr) {
          list = PaintLayerAnchorPointList::MaskStack;
          insert_after = active_item;
        }
        else if (active_effect != nullptr) {
          list = PaintLayerAnchorPointList::Effects;
          insert_after = active_effect;
        }
        else {
          list = PaintLayerAnchorPointList::Effects;
        }
      }
      if (point_owner == nullptr && list != PaintLayerAnchorPointList::Row) {
        BKE_report(CTX_wm_reports(&C),
                   RPT_WARNING,
                   is_anchor_point ? "Select a layer to add an Anchor Point to" :
                                     "Select a layer to add an Anchor Point Reader to");
        return -1;
      }
      anchor = point_owner;
      params.anchor_point_list = list;
      params.insert_after = insert_after;
    }
    const bool is_mask_correction = ELEM(params.kind,
                                         PaintLayerAddKind::MaskCorrection,
                                         PaintLayerAddKind::MaskCorrectionFill,
                                         PaintLayerAddKind::MaskCorrectionMaterial,
                                         PaintLayerAddKind::MaskFilter,
                                         PaintLayerAddKind::AnchorPointMaskCorrection,
                                         PaintLayerAddKind::MaskGradient);
    const bool is_correction = is_mask_correction || is_filter ||
                               ELEM(params.kind,
                                    PaintLayerAddKind::Correction,
                                    PaintLayerAddKind::CorrectionFill,
                                    PaintLayerAddKind::CorrectionMaterial,
                                    PaintLayerAddKind::AnchorPointCorrection,
                                    PaintLayerAddKind::GradientCorrection,
                                    PaintLayerAddKind::CurvePatchEffect);
    if (anchor != nullptr && !is_correction) {
      /* A correction is not a place a row can be added next to: the row it hangs on is. */
      if (MaterialPaintLayer *parent = BKE_paint_layers_correction_owner(material, *anchor)) {
        anchor = parent;
      }
    }
    if (is_correction && anchor == nullptr) {
      /* No row was named: the correction goes on the one being worked on. */
      anchor = BKE_paint_layers_active_layer_get(material);
      if (anchor == nullptr) {
        BKE_report(CTX_wm_reports(&C), RPT_WARNING, "Select a layer to add a correction to");
        return -1;
      }
    }

    params.anchor = anchor;
    /* An Add onto a folder means filling it: the policy turns this into Into unless the caller
     * aimed a line beside the folder. */
    params.place = (args.place == StackMovePlace::Below) ? PaintLayerPlace::Below :
                                                           PaintLayerPlace::Above;
    params.fill_folder = args.fill_folder;
    params.empty = args.empty;
    params.channels = args.channel_mask;
    if (ELEM(params.kind,
             PaintLayerAddKind::Fill,
             PaintLayerAddKind::CorrectionFill,
             PaintLayerAddKind::MaskCorrectionFill) &&
        args.color != nullptr)
    {
      params.fill_color = args.color;
    }
    MaterialPaintLayer *created = BKE_paint_layers_add_with_policy(material, params);
    if (created == nullptr) {
      if (is_anchor_point || is_anchor_point_reader) {
        BKE_report(CTX_wm_reports(&C),
                   RPT_WARNING,
                   "Could not add the Anchor Point to this layer");
      }
      if (is_adjustment) {
        BKE_report(CTX_wm_reports(&C),
                   RPT_WARNING,
                   "An adjustment needs a Pass Through folder or the top of the stack");
      }
      if (reads_material) {
        BKE_report(CTX_wm_reports(&C),
                   RPT_WARNING,
                   "That material cannot be read here: it is this material, or it already reads it");
      }
      if (is_anchor_point || is_anchor_point_reader) {
        BKE_report(CTX_wm_reports(&C), RPT_WARNING, "Select a layer to add an Anchor Point to");
      }
      return -1;
    }
    /* A new layer is painted on its content, not on the mask the previous one was edited in; a mask
     * correction is the mask the user means to paint. */
    const bool anchor_in_mask = (is_anchor_point || is_anchor_point_reader) &&
                                params.anchor_point_list == PaintLayerAnchorPointList::MaskStack;
    sculpt_paint::paint_stack_target_mode_set(C, (is_mask_correction || anchor_in_mask) ? PAINT_LAYER_TARGET_MASK :
                                                                       PAINT_LAYER_TARGET_CONTENT);
    if (is_anchor_point || is_anchor_point_reader) {
      /* The point or reader the user just added is the one they mean to work on. */
      BKE_paint_layers_active_set(material, created->marker);
      sculpt_paint::paint_stack_texpaint_slots_refresh(C);
    }
    else if (is_correction) {
      /* The correction the user just added is the one they mean to work on: paint into, or for a
       * filter set the parameters and channels of. A filter has no map, so a stroke on it lands
       * nowhere. */
      BKE_paint_layers_active_set(material, created->marker);
      sculpt_paint::paint_stack_texpaint_slots_refresh(C);
    }
    else if (params.kind == PaintLayerAddKind::Paint || params.kind == PaintLayerAddKind::Gradient ||
             is_adjustment || is_material)
    {
      /* The target resolves through the active row, and a plain Add leaves the previous one active
       * until the caller activates the new row, so the maps below would land on the wrong layer. */
      BKE_paint_layers_active_set(material, created->marker);
    }
    if (params.kind == PaintLayerAddKind::CurvePatchEffect && created->curve_patch != nullptr) {
      /* The effect starts with one seeded control curve laid out from its snapshot brush, so the
       * passive overlay and the tool entry have something to show and edit. The channel maps are
       * made when the tool is entered. */
      blender::ed::sculpt_paint::ED_paint_layers_curve_patch_row_seed_curve(*created->curve_patch);
      /* A fresh effect is the one being worked on: in Sculpt Mode its tool opens right away. */
      blender::ed::sculpt_paint::curve_patch_layer_enter_if_sculpting(C, material, *created);
    }
    sculpt_paint::paint_stack_gradient_tool_activate(C, created);
    /* A row with no map makes the first stroke create it and regenerate the graph before it can
     * write, which loses that stroke. The active slot's map is made together with the row, so the
     * first stroke already lands in it; the other channels still get theirs when they are used. */
    if (ELEM(params.kind,
             PaintLayerAddKind::Paint,
             PaintLayerAddKind::Correction,
             PaintLayerAddKind::MaskCorrection))
    {
      if (BKE_paint_layers_active_layer_get(material) != nullptr) {
        const Scene *scene = CTX_data_scene(&C);
        const PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
        const int slot_channel = BKE_paint_layers_texpaint_slot_channel(material.paint_active_slot);
        PaintLayersTarget target;
        if (BKE_paint_layers_target_get(
                material,
                is_mask_correction || slot_channel < 0 ? int(PAINT_MATERIAL_CHANNEL_BASE_COLOR) :
                                                         slot_channel,
                BKE_paint_layers_target_mode(&paint_mode),
                target) &&
            BKE_paint_layers_target_image(target) == nullptr)
        {
          BKE_paint_layers_target_ensure_writable(
              *CTX_data_main(&C), target, paint_mode.new_channel_image_size);
          sculpt_paint::paint_stack_texpaint_slots_refresh(C);
        }
      }
    }
    /* The other channels of a Paint layer are not made here: every paint entry point makes the
     * maps its stroke writes before the stroke starts (as a persistent side effect, not part of
     * that stroke's undo step), so a layer nothing is painted into holds one map instead of one
     * per channel of the set. A 4K map is 64 MiB of RAM and ~85 MiB of GPU memory each. */
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return paint_layers_ordinal_of(material, created);
  }

  bool row_remove(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
    if (layer == nullptr) {
      return false;
    }
    /* A correction is not a row of the stack: it leaves through its own call. */
    if (BKE_paint_layers_correction_owner(material, *layer) != nullptr) {
      if (!BKE_paint_layers_correction_remove(material, layer)) {
        return false;
      }
      sculpt_paint::paint_stack_texpaint_slots_refresh(C);
      WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
      return true;
    }
    if (layer->role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
      /* The row it hangs on becomes the active one again, so the mode stays on its mask. */
      if (!BKE_paint_layers_mask_item_remove(material, layer)) {
        return false;
      }
      sculpt_paint::paint_stack_texpaint_slots_refresh(C);
      WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
      return true;
    }
    layers_mask_edit_end_if_active(C, material, *layer);
    if (!BKE_paint_layers_remove(material, layer)) {
      return false;
    }
    /* Drop the object mask records of the removed row (and its descendants) right away. */
    BKE_paint_layer_object_masks_cleanup_all(*CTX_data_main(&C));
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool row_move(bContext &C,
                const StackFocus & /*focus*/,
                ID &owner,
                const int from_ordinal,
                const int anchor_ordinal,
                const StackMovePlace place,
                int *r_ordinal) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *from = paint_description_row_for_ordinal(material, from_ordinal);
    MaterialPaintLayer *anchor = paint_description_row_for_ordinal(material, anchor_ordinal);
    if (from == nullptr || anchor == nullptr) {
      return false;
    }
    const PaintLayerPlace layer_place = (place == StackMovePlace::Below) ? PaintLayerPlace::Below :
                                        (place == StackMovePlace::Into) ? PaintLayerPlace::Into :
                                                                          PaintLayerPlace::Above;
    if (!BKE_paint_layers_edit_move(material, *from, anchor, layer_place)) {
      if (place == StackMovePlace::Into) {
        /* The drop did nothing; without a word it reads as a dead drag. */
        BKE_report(CTX_wm_reports(&C), RPT_INFO, "Only layers of the same list can be grouped");
      }
      return false;
    }
    /* The ordinals past the move changed; the caller keeps the moved row selected by its new one. */
    if (r_ordinal != nullptr) {
      *r_ordinal = paint_layers_ordinal_of(material, from);
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  /* ---------------------------------------------------------------- */
  /** \name StackGroupingEditor
   *
   * The mask half and the folder verbs; colour tags arrive with their feature, so their verb keeps
   * the default that refuses.
   * \{ */

  const StackGroupingEditor *grouping() const override
  {
    return this;
  }

  StringRefNull aux_section_id() const override
  {
    return "MASK";
  }

  bool row_aux_set(bContext &C,
                   const StackFocus & /*focus*/,
                   ID &owner,
                   const int ordinal,
                   const bool add,
                   const float initial_color[4]) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
    if (layer == nullptr) {
      return false;
    }
    if (!add) {
      layers_mask_edit_end_if_active(C, material, *layer);
      if (!BKE_paint_layers_mask_remove(material, layer)) {
        return false;
      }
      WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
      return true;
    }
    if (BKE_paint_layers_mask_base(*layer) != nullptr) {
      BKE_report(CTX_wm_reports(&C), RPT_WARNING, "This layer already has a mask");
      return false;
    }
    /* The constant is the colour's grey: white shows the row, black hides it until painted in. */
    const float value = (initial_color != nullptr) ? initial_color[0] : 1.0f;
    if (BKE_paint_layers_mask_add(material, layer, value) == nullptr) {
      return false;
    }
    /* A mask the user just added is the one they mean to work on. */
    BKE_paint_layers_active_set(material, layer->marker);
    sculpt_paint::paint_stack_target_mode_set(C, PAINT_LAYER_TARGET_MASK);
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool row_aux_copy(bContext &C,
                    const StackFocus & /*focus*/,
                    ID &owner,
                    const int source_ordinal,
                    const int target_ordinal) const override
  {
    Material &material = layers_owner(owner);
    Main *bmain = CTX_data_main(&C);
    const MaterialPaintLayer *source = paint_description_row_for_ordinal(material, source_ordinal);
    MaterialPaintLayer *target = paint_description_row_for_ordinal(material, target_ordinal);
    /* A correction has no mask of its own; the one worked on is its layer's. */
    if (target != nullptr) {
      if (MaterialPaintLayer *owner = BKE_paint_layers_correction_owner(material, *target)) {
        target = owner;
      }
    }
    if (bmain == nullptr || source == nullptr || target == nullptr ||
        !BKE_paint_layers_mask_copy(*bmain, material, *source, *target))
    {
      return false;
    }
    sculpt_paint::paint_stack_texpaint_slots_refresh(C);
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  int row_merge_down(bContext &C,
                     const StackFocus & /*focus*/,
                     ID &owner,
                     const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *upper = paint_description_row_for_ordinal(material, ordinal);
    /* The row below in the compositing sense is the previous one of the same list, not the previous
     * ordinal: that one can be the last row of a folder that sits below. */
    MaterialPaintLayer *lower = (upper != nullptr) ? upper->prev : nullptr;
    if (upper == nullptr || lower == nullptr) {
      return -1;
    }
    MaterialPaintLayer *folder = BKE_paint_layers_edit_merge_down(material, *upper, *lower);
    if (folder == nullptr) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Cannot merge this layer down");
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return paint_layers_ordinal_of(material, folder);
  }

  /** The corrections at \a ordinals; empty when one of them is not a row of the description. */
  static Vector<MaterialPaintLayer *> corrections_for_ordinals(Material &material,
                                                               const Span<int> ordinals)
  {
    Vector<MaterialPaintLayer *> rows;
    for (const int ordinal : ordinals) {
      MaterialPaintLayer *row = paint_description_row_for_ordinal(material, ordinal);
      if (row == nullptr) {
        return {};
      }
      rows.append_non_duplicates(row);
    }
    return rows;
  }

  /** Whether \a object has \a ma in one of its material slots, object overrides included. */
  static bool object_uses_material(const Object &object, const Material &ma)
  {
    Object *object_mut = const_cast<Object *>(&object);
    for (int slot = 0; slot < object.totcol; slot++) {
      if (BKE_object_material_get(object_mut, short(slot + 1)) == &ma) {
        return true;
      }
    }
    return false;
  }

  /**
   * Whether \a a and \a b shade the same surface: same mesh data and the same modifier stack, so a
   * linked duplicate does not warn about a Gradient line it shares.
   */
  static bool object_same_bake_geometry(const Object &a, const Object &b)
  {
    if (a.data == nullptr || a.data != b.data) {
      return false;
    }
    const ModifierData *md_a = static_cast<const ModifierData *>(a.modifiers.first);
    const ModifierData *md_b = static_cast<const ModifierData *>(b.modifiers.first);
    while (md_a != nullptr && md_b != nullptr) {
      if (md_a->type != md_b->type) {
        return false;
      }
      md_a = md_a->next;
      md_b = md_b->next;
    }
    return md_a == nullptr && md_b == nullptr;
  }

  /**
   * Resolve the evaluated object a 3D Gradient correction lays through, the way the Image Editor's
   * 2D preview resolves it (#ED_space_image_gradient_preview_sync): the active mesh that uses
   * \a material, its evaluated object and evaluated mesh. False, with the reason reported, when
   * the selection needs one and the context cannot name it; \a r_surface stays null when the
   * selection needs none.
   */
  static bool corrections_bake_surface_resolve(bContext &C,
                                               Material &material,
                                               const Span<MaterialPaintLayer *> rows,
                                               CorrectionsBakeSurface &r_surface,
                                               Object *&r_active_object)
  {
    r_surface = {};
    r_active_object = nullptr;
    if (!BKE_paint_layers_corrections_surface_needed(material, rows)) {
      return true;
    }
    ReportList *reports = CTX_wm_reports(&C);
    Object *active_object = CTX_data_active_object(&C);
    if (active_object == nullptr) {
      BKE_report(reports,
                 RPT_ERROR,
                 "A Gradient correction in Object or Generated space needs the object it was "
                 "drawn in: no object is active");
      return false;
    }
    if (active_object->type != OB_MESH || !object_uses_material(*active_object, material)) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "A Gradient correction in Object or Generated space needs the object it was "
                  "drawn in: the active object \"%s\" is not a mesh that uses this material",
                  active_object->id.name + 2);
      return false;
    }
    const Depsgraph *depsgraph = CTX_data_expect_evaluated_depsgraph(&C);
    const Object *object_eval =
        depsgraph != nullptr ? DEG_get_evaluated<Object>(depsgraph, active_object) : nullptr;
    const Mesh *mesh =
        object_eval != nullptr ? BKE_object_get_evaluated_mesh(object_eval) : nullptr;
    if (mesh == nullptr) {
      BKE_report(reports,
                 RPT_ERROR,
                 "The active object's evaluated mesh is not available yet; let the dependency "
                 "graph update and try again");
      return false;
    }
    if (BKE_paint_layers_gradient_uv_map_name(*mesh).is_empty()) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "The active object \"%s\" has no UV map a Gradient correction in Object or "
                  "Generated space could be baked over",
                  active_object->id.name + 2);
      return false;
    }
    r_active_object = active_object;
    r_surface.object = object_eval;
    r_surface.mesh = mesh;
    return true;
  }

  /**
   * Warn that other objects of the file also use the material: each shades its own Object or
   * Generated line, so the merged map, made over \a surface, may not match them.
   */
  static void corrections_bake_report_other_users(bContext &C,
                                                  Main &bmain,
                                                  const Material &material,
                                                  const CorrectionsBakeSurface &surface,
                                                  const Object *active_object)
  {
    if (surface.object == nullptr) {
      return;
    }
    std::string other_users;
    int other_users_num = 0;
    for (Object &other : bmain.objects) {
      if (other.type != OB_MESH || &other == active_object ||
          !object_uses_material(other, material))
      {
        continue;
      }
      if (active_object != nullptr && object_same_bake_geometry(other, *active_object)) {
        continue;
      }
      other_users_num++;
      if (other_users_num <= 3) {
        if (!other_users.empty()) {
          other_users += ", ";
        }
        other_users += other.id.name + 2;
      }
    }
    if (other_users_num > 3) {
      other_users += ", ...";
    }
    if (other_users_num == 0) {
      return;
    }
    BKE_reportf(CTX_wm_reports(&C),
                RPT_WARNING,
                "The material is also used by other objects (%s), whose Object or Generated "
                "line may differ from \"%s\"",
                other_users.c_str(),
                surface.object->id.name + 2);
  }

  int rows_correction_bake_kind(bContext & /*C*/,
                                const StackFocus & /*focus*/,
                                ID &owner,
                                const Span<int> ordinals) const override
  {
    Material &material = layers_owner(owner);
    Vector<MaterialPaintLayer *> rows = corrections_for_ordinals(material, ordinals);
    return rows.is_empty() ? 0 : BKE_paint_layers_corrections_bake_kind(material, rows);
  }

  int rows_correction_bake(bContext &C,
                           const StackFocus & /*focus*/,
                           ID &owner,
                           const Span<int> ordinals) const override
  {
    Material &material = layers_owner(owner);
    Main *bmain = CTX_data_main(&C);
    Vector<MaterialPaintLayer *> rows = corrections_for_ordinals(material, ordinals);
    if (bmain == nullptr || rows.is_empty()) {
      return -1;
    }
    /* A correction being painted in Mask mode would be left pointing at a row that is gone. */
    for (MaterialPaintLayer *row : rows) {
      if (MaterialPaintLayer *row_owner = BKE_paint_layers_correction_owner(material, *row)) {
        layers_mask_edit_end_if_active(C, material, *row_owner);
      }
    }
    /* The shape comes first: a set that cannot be baked at all is refused for that, not for the
     * object a 3D Gradient would need. */
    if (BKE_paint_layers_corrections_bake_kind(material, rows) == 0) {
      BKE_paint_layers_corrections_bake_precheck(material, rows, CTX_wm_reports(&C));
      return -1;
    }
    /* An Object or a Generated Gradient reads the surface of a concrete object, which only an
     * object can name: the active one, resolved the way the Image Editor's 2D preview resolves it
     * (#ED_space_image_gradient_preview_sync). */
    CorrectionsBakeSurface surface;
    Object *active_object = nullptr;
    if (!corrections_bake_surface_resolve(C, material, rows, surface, active_object)) {
      return -1;
    }
    /* The refusals that need no render come first: a render is the expensive part. */
    if (!BKE_paint_layers_corrections_bake_precheck(
            material, rows, CTX_wm_reports(&C), surface))
    {
      return -1;
    }
    /* A Material correction shows most of its channels live through the shader graph, which the CPU
     * bake cannot read: only the maps of a render do. Render them now, at the row's own size, and
     * wait: the bake below reads them at once. */
    Vector<bUUID> render_rows;
    for (const MaterialPaintLayer *row : rows) {
      if (row->source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
        render_rows.append(row->marker);
      }
    }
    if (!render_rows.is_empty()) {
      material_bake::material_bake_layered_rows_ensure(
          *bmain, material, /*force_preview=*/true, render_rows, /*blocking=*/true);
    }
    MaterialPaintLayer *merged = BKE_paint_layers_corrections_bake(
        *bmain, material, rows, CTX_wm_reports(&C), surface);
    if (merged == nullptr) {
      return -1;
    }
    if (surface.object != nullptr) {
      /* The merged map reads one object's geometry: reproducible, and worth saying whose. */
      BKE_reportf(CTX_wm_reports(&C),
                  RPT_INFO,
                  "Merged the corrections over the object \"%s\"",
                  surface.object->id.name + 2);
      /* A warning, not a refusal: the merge is reproducible over the object it names, but
       * another user of the material shades its own Object or Generated line. */
      corrections_bake_report_other_users(C, *bmain, material, surface, active_object);
    }
    sculpt_paint::paint_stack_texpaint_slots_refresh(C);
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return paint_layers_ordinal_of(material, merged);
  }

  int rows_group(bContext &C,
                 const StackFocus & /*focus*/,
                 ID &owner,
                 const int from_ordinal,
                 const int to_ordinal) const override
  {
    Material &material = layers_owner(owner);
    const int low = std::min(from_ordinal, to_ordinal);
    const int high = std::max(from_ordinal, to_ordinal);
    if (low < 0) {
      return -1;
    }
    /* The run is by ordinal, which numbers a folder before what it holds: a folder inside the run
     * carries its children along, so they are not listed again. */
    Vector<MaterialPaintLayer *> rows;
    for (int ordinal = low; ordinal <= high; ordinal++) {
      MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
      if (layer == nullptr) {
        return -1;
      }
      /* A correction goes with the row it hangs on. */
      if (BKE_paint_layers_correction_owner(material, *layer) != nullptr) {
        continue;
      }
      /* Any ancestor already listed carries the row, not only its direct parent: a folder nested two
       * deep has its grandchildren in the run too. */
      const bool carried = std::any_of(
          rows.begin(), rows.end(), [&](const MaterialPaintLayer *root) {
            return root != layer && BKE_paint_layers_subtree_contains(*root, layer->marker);
          });
      if (carried) {
        continue;
      }
      rows.append(layer);
    }
    MaterialPaintLayer *folder = BKE_paint_layers_edit_group_range(material, rows);
    if (folder == nullptr) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Only layers of one list can be grouped");
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return paint_layers_ordinal_of(material, folder);
  }

  int row_ungroup(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *folder = paint_description_row_for_ordinal(material, ordinal);
    if (folder == nullptr || !BKE_paint_layers_is_folder(*folder)) {
      return -1;
    }
    const int child_num = BLI_listbase_count(&folder->children);
    layers_mask_edit_end_if_active(C, material, *folder);
    if (!BKE_paint_layers_ungroup(material, folder)) {
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return child_num;
  }

  int group_add(bContext &C,
                const StackFocus & /*focus*/,
                ID &owner,
                const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *anchor = (ordinal < 0) ?
                                     nullptr :
                                     paint_description_row_for_ordinal(material, ordinal);
    if (ordinal >= 0 && anchor == nullptr) {
      return -1;
    }
    MaterialPaintLayer *folder = BKE_paint_layers_edit_group_add(material, anchor);
    if (folder == nullptr) {
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return paint_layers_ordinal_of(material, folder);
  }

  bool row_aux_toggle(bContext &C,
                      const StackFocus & /*focus*/,
                      ID &owner,
                      const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
    MaterialPaintLayer *base = (layer != nullptr) ? BKE_paint_layers_mask_base(*layer) : nullptr;
    if (base == nullptr) {
      return false;
    }
    const bool enable = (base->flag & MA_PAINT_LAYER_ENABLED) == 0;
    if (!enable) {
      /* A switched-off mask hides nothing, so a stroke into it would be invisible. */
      layers_mask_edit_end_if_active(C, material, *layer);
    }
    if (!BKE_paint_layers_set_enabled(material, base, enable)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool row_color_tag_set(bContext &C,
                         const StackFocus & /*focus*/,
                         ID &owner,
                         const int ordinal,
                         const int color_tag) const override
  {
    Material &material = layers_owner(owner);
    if (!paint_layers_edit_color_tag(material, ordinal, color_tag)) {
      return false;
    }
    /* A list-drawing change only: the Outliner is the one view that shows it, and nothing
     * evaluated reads the tag. #stack_mutate sends the same notifier for the row edit; this keeps
     * the seam honest for a caller that reaches it another way. */
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
    return true;
  }

  /** \} */

  bool row_set_enabled(bContext &C,
                       const StackFocus & /*focus*/,
                       ID &owner,
                       const int ordinal,
                       const bool enable) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
    if (layer == nullptr || !BKE_paint_layers_set_enabled(material, layer, enable)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool row_set_locked(bContext &C,
                      const StackFocus & /*focus*/,
                      ID &owner,
                      const int ordinal,
                      const bool locked) const override
  {
    Material &material = layers_owner(owner);
    MaterialPaintLayer *layer = paint_description_row_for_ordinal(material, ordinal);
    if (layer == nullptr || !BKE_paint_layers_set_locked(material, layer, locked)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING_DRAW, &material.id);
    return true;
  }

  bool row_rename(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal,
                  const StringRefNull name) const override
  {
    Material &material = layers_owner(owner);
    if (!paint_layers_edit_rename(material, ordinal, name.c_str())) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Cannot rename this row");
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  int row_duplicate(bContext &C,
                    const StackFocus & /*focus*/,
                    ID &owner,
                    const int ordinal) const override
  {
    Material &material = layers_owner(owner);
    Main *bmain = CTX_data_main(&C);
    if (bmain == nullptr) {
      return -1;
    }
    const int copy = paint_layers_edit_duplicate(*bmain, material, ordinal);
    if (copy < 0) {
      return -1;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return copy;
  }

  int row_paste_correction(bContext &C,
                           const StackFocus & /*focus*/,
                           ID &owner,
                           const int source_ordinal,
                           const int target_ordinal) const override
  {
    Material &material = layers_owner(owner);
    Main *bmain = CTX_data_main(&C);
    const MaterialPaintLayer *source = paint_description_row_for_ordinal(material, source_ordinal);
    if (source == nullptr || !ELEM(source->role,
                                   MA_PAINT_LAYER_ROLE_EFFECT,
                                   MA_PAINT_LAYER_ROLE_MASK_ITEM))
    {
      return STACK_PASTE_NOT_APPLICABLE;
    }
    MaterialPaintLayer *target = paint_description_row_for_ordinal(material, target_ordinal);
    if (target != nullptr) {
      /* A correction or a mask item is no place to paste onto: the layer it hangs on is. */
      if (MaterialPaintLayer *parent = BKE_paint_layers_correction_owner(material, *target)) {
        target = parent;
      }
      else if (target->role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
        target = BKE_paint_layers_mask_item_owner(material, *target);
      }
    }
    if (bmain == nullptr || target == nullptr) {
      return -1;
    }
    MaterialPaintLayer *copy = BKE_paint_layers_correction_paste(*bmain, material, *source, target);
    if (copy == nullptr) {
      BKE_report(CTX_wm_reports(&C),
                 RPT_INFO,
                 "A mask correction needs a layer that has a mask to lay over");
      return -1;
    }
    /* What was pasted is what the user works on next, in the mode it belongs to. */
    BKE_paint_layers_active_set(material, copy->marker);
    sculpt_paint::paint_stack_target_mode_set(C,
                           copy->role == MA_PAINT_LAYER_ROLE_MASK_ITEM ? PAINT_LAYER_TARGET_MASK :
                                                                         PAINT_LAYER_TARGET_CONTENT);
    sculpt_paint::paint_stack_texpaint_slots_refresh(C);
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return paint_layers_ordinal_of(material, copy);
  }

  /** \} */

 private:
  /** Drop handler with the built-in actions and the FileHandler fallback. */
  std::unique_ptr<StackDropHandler> drop_handler_ = paint_layers_drop_handler_create();
};

std::unique_ptr<StackSource> stack_source_paint_layers_create()
{
  return std::make_unique<PaintLayersStackSource>();
}

/* -------------------------------------------------------------------- */
/** \name Assign Dropped Image to a Channel
 *
 * The popup an image drop opens: whichever channel the user picks is where the image lands -- on
 * the Fill layer it was dropped on, or on a new Fill layer beside the row it was aimed at (on top
 * when it named none).
 * \{ */

int paint_channel_icon(const int channel)
{
  switch (eMaterialPaintChannel(channel)) {
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
      return ICON_IMAGE_RGB;
    case PAINT_MATERIAL_CHANNEL_METALLIC:
      return ICON_NODE_MATERIAL;
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
      return ICON_MOD_NOISE;
    case PAINT_MATERIAL_CHANNEL_SPECULAR:
      return ICON_INDIRECT_ONLY_ON;
    case PAINT_MATERIAL_CHANNEL_NORMAL:
      return ICON_NORMALS_FACE;
    case PAINT_MATERIAL_CHANNEL_HEIGHT:
      return ICON_MOD_DISPLACE;
    case PAINT_MATERIAL_CHANNEL_ALPHA:
      return ICON_IMAGE_ALPHA;
    case PAINT_MATERIAL_CHANNEL_AO:
      return ICON_SHADING_RENDERED;
    case PAINT_MATERIAL_CHANNEL_EMISSION:
      return ICON_LIGHT;
    case PAINT_MATERIAL_CHANNEL_CUSTOM:
      break;
  }
  return ICON_IMAGE_DATA;
}

const EnumPropertyItem *stack_channel_image_assign_channel_itemf(bContext * /*C*/,
                                                                 PointerRNA * /*ptr*/,
                                                                 PropertyRNA * /*prop*/,
                                                                 bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  /* The identifiers are the channel enum's own, so a script names a channel the same way here as
   * everywhere else; only the icon differs from that enum. */
  for (const EnumPropertyItem *channel_item = rna_enum_material_paint_channel_items;
       channel_item->identifier != nullptr;
       channel_item++)
  {
    if (channel_item->identifier[0] == '\0' ||
        channel_item->value == PAINT_MATERIAL_CHANNEL_CUSTOM)
    {
      continue;
    }
    EnumPropertyItem item = *channel_item;
    item.icon = paint_channel_icon(channel_item->value);
    RNA_enum_item_add(&items, &items_num, &item);
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

static wmOperatorStatus stack_channel_image_assign_invoke(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent * /*event*/)
{
  ui::PopupMenu *pup = ui::popup_menu_begin(C, IFACE_("Assign to Channel"), ICON_NONE);
  ui::Layout &layout = *ui::popup_menu_layout(pup);
  /* Each item calls this operator again with one channel chosen, in exec context -- re-invoking
   * would open this popup again. The extra properties carry the image and the target along. */
  layout.operator_context_set(wm::OpCallContext::ExecDefault);
  PointerRNA extra = layout.op_menu_enum(C, op->type, "channel", std::nullopt, ICON_NONE);
  RNA_int_set(&extra, "image_uid", RNA_int_get(op->ptr, "image_uid"));
  char marker_str[UUID_STRING_SIZE];
  RNA_string_get(op->ptr, "layer", marker_str);
  RNA_string_set(&extra, "layer", marker_str);
  RNA_string_get(op->ptr, "anchor", marker_str);
  RNA_string_set(&extra, "anchor", marker_str);
  RNA_boolean_set(&extra, "below", RNA_boolean_get(op->ptr, "below"));
  ui::popup_menu_end(C, pup);
  return OPERATOR_INTERFACE;
}

/** The layer a marker property names, or null when it is empty or names nothing. */
static MaterialPaintLayer *stack_channel_image_assign_marker_layer(Material &material,
                                                                   wmOperator &op,
                                                                   const char *prop_name)
{
  char marker_str[UUID_STRING_SIZE];
  RNA_string_get(op.ptr, prop_name, marker_str);
  bUUID marker;
  if (marker_str[0] == '\0' || !BLI_uuid_parse_string(&marker, marker_str)) {
    return nullptr;
  }
  return BKE_paint_layers_find(material, marker);
}

static wmOperatorStatus stack_channel_image_assign_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);
  ID *owner = outliner_stack_owner_get(outliner_stack_read_context(*C), *space_outliner);
  if (owner == nullptr || GS(owner->name) != ID_MA) {
    return OPERATOR_CANCELLED;
  }
  Material &material = layers_owner(*owner);
  if (!stack_clipboard_material_editable_check(material, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  Image *image = id_cast<Image *>(
      BKE_libblock_find_session_uid(bmain, ID_IM, RNA_int_get(op->ptr, "image_uid")));
  if (image == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "The dropped image is gone");
    return OPERATOR_CANCELLED;
  }
  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  if (!stack_image_assign_check(material, nullptr, channel, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  MaterialPaintLayer *layer = nullptr;
  bool created = false;
  bool created_correction = false;
  if (RNA_string_length(op->ptr, "layer") > 0) {
    layer = stack_channel_image_assign_marker_layer(material, *op, "layer");
    if (layer == nullptr) {
      BKE_report(op->reports, RPT_ERROR, "The layer the image was dropped on is gone");
      return OPERATOR_CANCELLED;
    }
    if (!stack_image_assign_check(material, layer, channel, op->reports)) {
      return OPERATOR_CANCELLED;
    }
  }
  else {
    /* A Fill layer of its own, named after the image without the file extension it may carry. The
     * channel record below carries the image, so the Fill reads it instead of its constant. */
    char name[MAX_NAME];
    STRNCPY(name, image->id.name + 2);
    BLI_path_extension_strip(name);
    MaterialPaintLayer *anchor = stack_channel_image_assign_marker_layer(material, *op, "anchor");
    /* The drop named a place with its line; an anchor folder does not swallow a row aimed beside
     * it, the way an Add button's Above would. */
    PaintLayerPlace place = (anchor != nullptr && RNA_boolean_get(op->ptr, "below")) ?
                                PaintLayerPlace::Below :
                                PaintLayerPlace::Above;
    if (anchor != nullptr) {
      if (MaterialPaintLayer *correction_owner = BKE_paint_layers_correction_owner(material,
                                                                                  *anchor))
      {
        /* Beside a correction the line means another correction of the row it hangs on, at that
         * place among them -- "between two corrections" included. A stack row cannot be linked
         * next to one, which is why this does not go through #BKE_paint_layers_add. */
        layer = BKE_paint_layers_correction_add(
            material, correction_owner, name, MA_PAINT_LAYER_SOURCE_CONSTANT);
        if (layer == nullptr || !BKE_paint_layers_edit_move(material, *layer, anchor, place)) {
          if (layer != nullptr) {
            BKE_paint_layers_correction_remove(material, layer);
          }
          BKE_report(op->reports, RPT_ERROR, "Could not add a correction for the image");
          return OPERATOR_CANCELLED;
        }
        created = true;
        created_correction = true;
      }
      else if (MaterialPaintLayer *mask_owner = BKE_paint_layers_mask_item_owner(material,
                                                                                 *anchor))
      {
        /* A mask item is no place for a stack row either: the row it belongs to is. */
        anchor = mask_owner;
      }
    }
    if (layer == nullptr) {
      /* Added bare, not through the Add policy: the policy gives a Fill the default channel set,
       * and the layer should take part in the one channel the user picked and no other. */
      layer = BKE_paint_layers_add(material, name, anchor, place);
      if (layer == nullptr) {
        BKE_report(op->reports, RPT_ERROR, "Could not add a layer for the image");
        return OPERATOR_CANCELLED;
      }
      layer->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
      created = true;
    }
  }

  if (BKE_paint_layers_channel_add(material, layer, channel) == nullptr ||
      !BKE_paint_layers_channel_set_image(material, layer, channel, image))
  {
    BKE_report(op->reports, RPT_ERROR, "The image could not be assigned to the channel");
    if (created) {
      /* A refused assignment must not read as a silent add. */
      if (created_correction) {
        BKE_paint_layers_correction_remove(material, layer);
      }
      else {
        BKE_paint_layers_remove(material, layer);
      }
    }
    return OPERATOR_CANCELLED;
  }
  if (created_correction && channel != PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    /* A Fill correction starts with a Base Color so a fresh one is visible; when the image landed
     * in another channel that flat colour must not cover the row's Base Color as well. */
    BKE_paint_layers_channel_remove(material, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
  /* What the Add wrapper does for a row it made: the cached rows go, the selection and the active
   * row move to the layer the image landed on, and the Outliner rebuilds. */
  const int ordinal = paint_layers_ordinal_of(material, layer);
  stack_row_edit_finished(*C, *space_outliner, material.id, ordinal, true);
  outliner_stack_row_activate(C, *space_outliner, ordinal);
  /* A correction is listed under its row's channels section: bring that section up so the
   * correction the drop just made is not there and unseen. */
  stack_row_reveal_attached(*space_outliner, ordinal);
  /* The edit helper above only tells the Outliner; the Properties editor and the previews show the
   * material. */
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material.id);
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_channel_image_assign(wmOperatorType *ot)
{
  ot->name = "Assign Image to Channel";
  ot->description =
      "Assign the dropped image to a channel of the layer, or add a new Fill layer or correction "
      "for it";
  ot->idname = "OUTLINER_OT_stack_layer_channel_image_assign";

  ot->invoke = stack_channel_image_assign_invoke;
  ot->exec = stack_channel_image_assign_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "channel", rna_enum_dummy_NULL_items, 0, "Channel", "Channel the image paints");
  RNA_def_enum_funcs(prop, stack_channel_image_assign_channel_itemf);
  /* A session UID is a uint32 read through an int, so the whole int range is its range: clamping
   * at INT32_MAX would fold every uid above it onto that value. */
  RNA_def_int(ot->srna,
              "image_uid",
              0,
              INT32_MIN,
              INT32_MAX,
              "Image",
              "",
              INT32_MIN,
              INT32_MAX);
  RNA_def_string(ot->srna,
                 "layer",
                 nullptr,
                 UUID_STRING_SIZE,
                 "Layer",
                 "Marker of the Fill layer the image was dropped on; empty adds a Fill layer of "
                 "its own");
  RNA_def_string(ot->srna,
                 "anchor",
                 nullptr,
                 UUID_STRING_SIZE,
                 "Anchor",
                 "Marker of the row or correction a new layer is placed beside; empty puts it on "
                 "top");
  RNA_def_boolean(ot->srna, "below", false, "Below", "Place a new layer below the anchor row");
}

/** \} */

}  // namespace blender::ed::outliner
