/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The paint layers of a material, as a stack.
 *
 * The layers are not stored anywhere: they are the shape of the material's node graph, a chain of
 * Mix nodes over Image Textures, and #BKE_paint_material_layer_stack_from_material reads them back
 * out of it. That is deliberate -- the graph is what renders, so anything else would be a second
 * truth to keep in sync -- and it is why this source is a reader rather than an owner.
 *
 * Activating a row points #PaintModeSettings.channel_image_bindings at the layer's maps, which is
 * scene data and undoable. Which material last did that is not: it is a fact about this session's
 * navigation, kept here so that only the Outliner showing the owning material claims a row is
 * active. Two Outliners pointed at different materials must not both look like the paint target.
 */

#include <climits>

#include <fmt/format.h>

#include "AS_asset_representation.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_uuid_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_layer_edit.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLI_assert.h"
#include "BLI_fileops.hh"
#include "BLI_hash.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_path_utils.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"

#include "DEG_depsgraph.hh"

#include "BLT_translation.hh"

#include "IMB_imbuf_types.hh"

#include "ED_asset_image_utils.hh"
#include "ED_asset_import.hh"
#include "ED_buttons.hh"
#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_paint_material_layer.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_types.hh"

#include "outliner_intern.hh"
#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

/**
 * The mask-editing half of a preview-slot click, testable without a #bContext (see
 * #PaintMaterialStackSource::preview_activate, which resolves \a scene from context and calls
 * this). \a paint is the Paint whose brush is swapped. \a section_id names which preview was
 * clicked ("MASK" or "CHANNELS", see #outliner_stack_preview_section_from_cursor); any other
 * value is a no-op.
 */
bool paint_material_mask_preview_activate(Main &bmain,
                                           Scene &scene,
                                           Paint &paint,
                                           const StackRow &row,
                                           const StringRef section_id);

/**
 * Whether the MASK content section of \a row shows \a mask_image -- or, for a mask correction
 * row, whether the image is the correction's own map (named by its preview slot). Testable
 * without a #bContext like #paint_material_mask_preview_activate above: row activation uses it
 * to decide if the mask being painted survives the switch.
 */
bool paint_row_owns_mask_image(const StackRow &row, const Image *mask_image);

/**
 * One row of the stack, by the ordinal that addresses it: a layer row itself, or one of the
 * corrections hanging off it.
 *
 * File-local like the two declarations above, and duplicated into the test file for the same
 * reason they are: the routes are what the tests have to check directly.
 */
struct PaintStackRowRoute {
  /** False for the row a layer or group itself gets, true for one of its corrections. */
  bool is_correction = false;
  /** The ordinal of the layer row the route stands for, or hangs off. */
  int layer_ordinal = -1;
  /** The correction's identity; nil for a layer route. */
  bUUID correction = {};
  /** The parent's section the correction hangs under; meaningless for a layer route. */
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
};

/**
 * The deterministic ordinal budget the rows are addressed by, shared by #rows_build and every
 * edit that routes an ordinal. Defined below, next to the other test-facing functions.
 */
Map<int, PaintStackRowRoute> paint_stack_routes_build(
    Span<PaintMaterialLayerStackEntry> entries, int &r_first_unaddressable_index);

/**
 * This source's own color session: the pixel-diff record a live fill-color edit captures the
 * layer's pristine pixels into. Generic code only carries the pointer between #StackColorEditor
 * methods; the tile map inside never leaves this file.
 */
struct StackColorSession {
  PaintTileMap *tiles = nullptr;
};

namespace {

Material *paint_material_get(const StackReadContext &ctx, const StackFocus &focus)
{
  Object *object = outliner_stack_focus_object_get(ctx, focus);
  if (object == nullptr) {
    return nullptr;
  }
  const short slot = focus.sub_index >= 0 ? short(focus.sub_index + 1) : object->actcol;
  return BKE_object_material_get(object, slot);
}

/** The owner of a paint stack is always a material; no other ID reaches this source. */
Material &paint_owner(ID &owner)
{
  BLI_assert(GS(owner.name) == ID_MA);
  return id_cast<Material &>(owner);
}

const Material &paint_owner(const ID &owner)
{
  BLI_assert(GS(owner.name) == ID_MA);
  return id_cast<const Material &>(owner);
}

/**
 * What #PaintMaterialStackSource::insert_anchor_get answers when the drop names a row that is no
 * longer there: distinct from -1, which is the stack's top and a perfectly good answer.
 */
constexpr int PAINT_STACK_INSERT_REFUSED = -2;

/**
 * The ordinal of the layer marked \a marker in \a material's current graph, or -1.
 *
 * Resolves through the material's own graph rather than the Outliner's cached rows: a drop
 * handler is handed a #StackDropPayload by the seam, not a #SpaceOutliner, and the marker is
 * exactly the identity this source already hands out as #StackRow::stable_id.
 */
int paint_layer_ordinal_for_marker(Main &bmain, const Material &material, const bUUID &marker)
{
  Vector<PaintMaterialLayerStackEntry> entries;
  if (!BKE_paint_material_layer_stack_from_material(bmain, material, entries)) {
    return -1;
  }
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    if (BLI_uuid_equal(entry.marker, marker)) {
      return entry.ordinal;
    }
  }
  return -1;
}

/**
 * Fold \a tree's topology -- every node's identifier and every link's two ends -- into \a r_hash,
 * across the group trees as well.
 *
 * Counts are not enough here: a link taken off one socket and put on another leaves the node and
 * link counts alone, and the rows are read from the links. Identifiers are per-tree-build values,
 * so a graph taken apart and made new hashes differently -- a needless re-read is the cheap wrong
 * to make; a stale one is the expensive wrong to keep.
 */
void paint_topology_hash(const bNodeTree &tree, Set<const bNodeTree *> &visited, uint64_t &r_hash)
{
  if (!visited.add(&tree)) {
    return;
  }
  for (const bNode &node : tree.nodes) {
    r_hash = r_hash * 1000003u ^ uint64_t(node.identifier) * 2654435761u;
    if (node.is_group() && node.id != nullptr) {
      paint_topology_hash(*id_cast<const bNodeTree *>(node.id), visited, r_hash);
    }
  }
  for (const bNodeLink &link : tree.links) {
    /* The ends can be null on a link that is mid-rewire; the node draw code guards the same
     * fields for the same reason. Such a link describes no topology yet, so it hashes nothing. */
    if (link.fromnode == nullptr || link.tonode == nullptr || link.fromsock == nullptr ||
        link.tosock == nullptr)
    {
      continue;
    }
    r_hash = r_hash * 1000003u ^
             (uint64_t(link.fromnode->identifier) * 40503u ^
              uint64_t(BLI_hash_string(link.fromsock->identifier)) * 2654435761u ^
              uint64_t(link.tonode->identifier) * 40503u ^
              uint64_t(BLI_hash_string(link.tosock->identifier)) * 2654435761u);
  }
}

/**
 * The icon a channel's map is listed with.
 *
 * A layer expands into up to ten maps that are all Images; without this they are ten identical
 * rows differing only by name.
 */
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

/** The layer's map for \a role, or null. Read back from the row rather than from the model. */
Image *paint_row_map_get(const StackRow &row, const int role)
{
  for (const StackContentSection &section : row.content_sections) {
    for (const StackSubRow &sub_row : section.sub_rows) {
      /* A switched-off channel's map is not a paint target (spec 6), and the bindings the toggle
       * leaves for it are null: activating and "is active" both have to agree with that. */
      if (sub_row.role == role && !sub_row.inactive) {
        return id_cast<Image *>(sub_row.id);
      }
    }
  }
  return nullptr;
}

/** Whether the row names a map at all. A group row names none: it is a folder, not a target. */
bool paint_row_has_map(const StackRow &row)
{
  for (const StackContentSection &section : row.content_sections) {
    for (const StackSubRow &sub_row : section.sub_rows) {
      if (sub_row.id != nullptr) {
        return true;
      }
    }
  }
  return false;
}

/**
 * Whether \a image is a generated, fully transparent tile that no brush has touched yet: the
 * data-block exists, but its thumbnail has nothing to show.
 *
 * It is the source's business to know this -- generic code only draws what #StackRowPreview
 * tells it -- and it has to stay in step with #state_hash, which folds the same answer in: a
 * first stroke flips it, and with it the hash that says the rows are stale.
 */
bool paint_image_is_blank(const Image &image)
{
  if (image.source != IMA_SRC_GENERATED) {
    return false;
  }
  const ImageTile *base_tile = BKE_image_get_tile(const_cast<Image *>(&image), 0);
  return base_tile != nullptr && base_tile->gen_type == IMA_GENTYPE_BLANK &&
         base_tile->gen_color[3] == 0.0f && !BKE_image_is_dirty(const_cast<Image *>(&image));
}

/** The channel maps a layer can show in its channels slot, masked out of the candidates. */
const Image *paint_channels_preview_get(const PaintMaterialLayerStackEntry &entry)
{
  const Image *preview = entry.channel_images.lookup_default(PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                             nullptr);
  if (preview == nullptr) {
    for (const int channel : BKE_paint_material_composite_passes()) {
      if (channel == PAINT_LAYER_MAP_MASK) {
        continue;
      }
      preview = entry.channel_images.lookup_default(channel, nullptr);
      if (preview != nullptr) {
        break;
      }
    }
  }
  return preview;
}

/** The slot a layer's channel maps sit behind: present even with no map, reserving the
 * empty-texture thumbnail so a mask added to an empty layer lands beside it, not in its place. */
StackRowPreview paint_channels_slot_build(const PaintMaterialLayerStackEntry &entry)
{
  StackRowPreview slot;
  slot.section_id = "CHANNELS";
  const Image *preview = paint_channels_preview_get(entry);
  if (preview != nullptr) {
    slot.id_uid = preview->id.session_uid;
    slot.id_type = ID_IM;
    slot.is_blank = paint_image_is_blank(*preview);
  }
  return slot;
}

/**
 * The mask slot of a row that has one, and the section it opens.
 *
 * \param keeps_row_icon: for a group, its folder icon stays ahead of the mask thumbnail. The
 * caller knows what the row is; the slot only records it for the draw.
 */
StackRowPreview paint_mask_slot_build(const bool keeps_row_icon, const bool enabled)
{
  StackRowPreview slot;
  slot.section_id = "MASK";
  /* A switched-off mask reads as one at a glance: the icon says the row's coverage no longer
   * comes from it. */
  slot.icon = enabled ? ICON_MOD_MASK : ICON_MOD_SUBSURF;
  slot.keeps_row_icon = keeps_row_icon;
  /* Whether the mask began black or white is unreadable once it has been painted over, so the
   * label stays neutral rather than guessing from the initial fill color. */
  slot.label = IFACE_("Mask");
  return slot;
}

/** The section a layer's channel maps expand into: every map but the mask, by channel role. */
StackContentSection paint_channels_section_build(const PaintMaterialLayerStackEntry &entry)
{
  StackContentSection section;
  section.identifier = "CHANNELS";
  section.name = IFACE_("Channel Textures");

  for (const auto item : entry.channel_images.items()) {
    if (item.value == nullptr || item.key >= STACK_ROW_SUB_ROW_STRIDE) {
      continue;
    }
    if (item.key == PAINT_LAYER_MAP_MASK) {
      continue;
    }
    StackSubRow sub_row;
    sub_row.role = item.key;
    sub_row.name = item.value->id.name + 2;
    sub_row.id = &item.value->id;
    sub_row.icon = paint_channel_icon(item.key);
    sub_row.inactive = item.key >= 0 && item.key < 32 &&
                       (entry.disabled_channels_mask & (uint32_t(1) << item.key)) != 0;
    section.sub_rows.append(std::move(sub_row));
  }
  return section;
}

/** The MASK section of a row whose mask is made of corrections only: nothing below it yet. */
StackContentSection paint_mask_section_empty_build()
{
  StackContentSection section;
  section.identifier = "MASK";
  section.name = IFACE_("Mask Content");
  return section;
}

/** The section a mask expands into: the mask image itself. */
StackContentSection paint_mask_section_build(const Image &mask_image)
{
  StackContentSection section = paint_mask_section_empty_build();

  StackSubRow sub_row;
  sub_row.role = PAINT_LAYER_MAP_MASK;
  sub_row.name = mask_image.id.name + 2;
  sub_row.id = const_cast<ID *>(&mask_image.id);
  /* The sub-row icon agrees with the mask slot and the row icon: switched off reads as
   * switched off everywhere. */
  sub_row.icon = (mask_image.paint_layer_mask_disabled == 0) ? ICON_MOD_MASK : ICON_MOD_SUBSURF;
  section.sub_rows.append(std::move(sub_row));
  return section;
}

/**
 * The route \a ordinal names in \a material's model as it stands now, or nullopt when it names no
 * row the tree could have shown. The mutating methods route on this: a correction row goes to the
 * correction API, a layer row to the layer API.
 */
std::optional<PaintStackRowRoute> paint_stack_route_get(Main &bmain,
                                                        Material &material,
                                                        const int ordinal)
{
  if (ordinal < 0) {
    return std::nullopt;
  }
  Vector<PaintMaterialLayerStackEntry> entries;
  if (!BKE_paint_material_layer_stack_from_material(bmain, material, entries)) {
    return std::nullopt;
  }
  int first_unaddressable = -1;
  const Map<int, PaintStackRowRoute> routes = paint_stack_routes_build(entries,
                                                                       first_unaddressable);
  const PaintStackRowRoute *route = routes.lookup_ptr(ordinal);
  if (route == nullptr) {
    return std::nullopt;
  }
  return *route;
}

/** The ordinal the correction \a marker is addressed by after an edit, or -1. */
int paint_correction_route_ordinal_get(Main &bmain, Material &material, const bUUID &marker)
{
  Vector<PaintMaterialLayerStackEntry> entries;
  if (!BKE_paint_material_layer_stack_from_material(bmain, material, entries)) {
    return -1;
  }
  int first_unaddressable = -1;
  const Map<int, PaintStackRowRoute> routes = paint_stack_routes_build(entries,
                                                                       first_unaddressable);
  for (const auto item : routes.items()) {
    if (item.value.is_correction && BLI_uuid_equal(item.value.correction, marker)) {
      return item.key;
    }
  }
  return -1;
}

std::optional<PaintMaterialCorrectionEffect> paint_correction_effect_get(
    const Main &bmain, const Material &material, const bUUID &marker)
{
  Vector<PaintMaterialLayerStackEntry> entries;
  if (!BKE_paint_material_layer_stack_from_material(bmain, material, entries)) {
    return std::nullopt;
  }
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    for (const Vector<PaintMaterialLayerCorrectionEntry> *corrections :
         {&entry.content_corrections, &entry.mask_corrections})
    {
      for (const PaintMaterialLayerCorrectionEntry &correction : *corrections) {
        if (BLI_uuid_equal(correction.marker, marker)) {
          return correction.effect;
        }
      }
    }
  }
  return std::nullopt;
}

/** One correction row: hangs off its parent layer's row, under the parent's section it adjusts. */
StackRow paint_correction_row_build(const PaintMaterialLayerStackEntry &parent,
                                    const PaintMaterialLayerCorrectionEntry &correction,
                                    const PaintStackRowRoute &route,
                                    const int route_ordinal,
                                    const int shown_channel)
{
  StackRow row;
  row.ordinal = int16_t(route_ordinal);
  row.depth = parent.depth + 1;
  row.parent_ordinal = parent.ordinal;
  row.parent_section_id = (route.section == PaintMaterialCorrectionSection::Mask) ? "MASK" :
                                                                                    "CHANNELS";
  row.stable_id = correction.marker;
  row.enabled = correction.enabled;
  row.supported = correction.supported;
  row.unsupported_reason = correction.supported ? nullptr : "Correction is not supported";
  row.name = correction.name;
  row.name_buffer = correction.label;
  row.icon = ICON_BRUSH_DATA;

  /* The same per-channel value and mode columns a layer row shows, read off the channel the
   * scene's picker names; a correction absent in that channel keeps the columns empty. */
  if (const PointerRNA *factor = correction.channel_factor_props.lookup_ptr(shown_channel)) {
    row.value_ptr = *factor;
    row.value_prop = "value";
  }
  if (const PointerRNA *blend = correction.channel_blend_props.lookup_ptr(shown_channel)) {
    row.mode_ptr = *blend;
    row.mode_prop = "blend_type";
  }

  /* One preview slot, showing the map the section it hangs under is about: a mask correction's
   * single shared map is tagged with the mask role, a content correction's map with the channel
   * it paints. A correction with no map there yet shows the empty thumbnail. */
  StackRowPreview slot;
  /* Only Paint corrections expose a clickable preview that can switch a brush target. Fill rows
   * keep the thumbnail as a channel indicator, but do not advertise a paint action. */
  slot.section_id = (correction.effect == PaintMaterialCorrectionEffect::Paint) ?
                        row.parent_section_id :
                        "";
  const int slot_channel = (route.section == PaintMaterialCorrectionSection::Mask) ?
                               int(PAINT_LAYER_MAP_MASK) :
                               shown_channel;
  const Image *map = correction.channel_images.lookup_default(slot_channel, nullptr);
  if (map != nullptr) {
    slot.id_uid = map->id.session_uid;
    slot.id_type = ID_IM;
    slot.is_blank = paint_image_is_blank(*map);
  }
  else {
    slot.is_blank = true;
  }
  row.preview_slots.append(std::move(slot));
  return row;
}

/**
 * The correction rows of \a entries, appended after every layer row of \a r_rows.
 *
 * The tree lists them under the parent's active content section, so their place in \a r_rows only
 * has to keep the parent-resolution walk from mistaking one for a group; they go out in the same
 * walk #paint_stack_routes_build allocates by, which is bottom to top of each parent's sections.
 * When the ordinal budget ran out, everything unaddressed collapses into the same stub row a
 * stack too large to display gets -- unless the layer walk already emitted it.
 */
void paint_correction_rows_append(Span<PaintMaterialLayerStackEntry> entries,
                                  const Set<int> &shown_layer_ordinals,
                                  const bool overflow_stub_emitted,
                                  const int shown_channel,
                                  Vector<StackRow> &r_rows)
{
  int first_unaddressable = -1;
  const Map<int, PaintStackRowRoute> routes = paint_stack_routes_build(entries,
                                                                       first_unaddressable);
  if (routes.is_empty()) {
    return;
  }
  /* Correction routes by marker: the walk below goes in the model's own order, which is the order
   * the budget allocated in, and asks each correction for the ordinal it was given. */
  Map<UUID, int> ordinal_by_marker;
  for (const auto item : routes.items()) {
    if (item.value.is_correction) {
      ordinal_by_marker.add(UUID(item.value.correction), item.key);
    }
  }

  int correction_index = 0;
  std::string stub_name;
  bool stub_needed = false;
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    for (const Vector<PaintMaterialLayerCorrectionEntry> *corrections :
         {&entry.content_corrections, &entry.mask_corrections})
    {
      for (const PaintMaterialLayerCorrectionEntry &correction : *corrections) {
        const int *route_ordinal = ordinal_by_marker.lookup_ptr(UUID(correction.marker));
        if (route_ordinal == nullptr) {
          /* Out of the budget: this and every correction after it in the walk has no ordinal the
           * tree can address. The first one is what the stub row stands in for. */
          stub_needed = true;
          if (correction_index == first_unaddressable) {
            stub_name = correction.name;
          }
        }
        else {
          const PaintStackRowRoute &route = routes.lookup(*route_ordinal);
          if (shown_layer_ordinals.contains(route.layer_ordinal)) {
            r_rows.append(paint_correction_row_build(
                entry, correction, route, *route_ordinal, shown_channel));
          }
        }
        correction_index++;
      }
    }
  }

  if (stub_needed && !overflow_stub_emitted) {
    StackRow row;
    row.ordinal = STACK_ROW_ORDINAL_MAX + 1;
    row.supported = false;
    row.unsupported_reason = "Stack is too large to display";
    row.name = std::move(stub_name);
    row.icon = ICON_IMAGE_RGB;
    r_rows.append(std::move(row));
  }
}

class PaintMaterialStackSource final : public StackSource,
                                       public StackEditor,
                                       public StackGroupingEditor,
                                       public StackColorEditor,
                                       public StackDropHandler {
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
    return material != nullptr && BKE_paint_material_has_layer_stack(*material);
  }

  ID *object_preview_id(const StackReadContext & /*ctx*/, Object &object) const override
  {
    /* An object row reads as its stack, and a paint stack is read from the active material. */
    Material *material = BKE_object_material_get(&object, object.actcol);
    return (material != nullptr) ? &material->id : nullptr;
  }

  ID *owner_get(const StackReadContext &ctx, const StackFocus &focus) const override
  {
    Material *material = paint_material_get(ctx, focus);
    return material != nullptr ? &material->id : nullptr;
  }

  void sub_selections_get(const StackReadContext &ctx,
                          const StackFocus &focus,
                          Vector<StackSubSelection> &r_items) const override
  {
    Object *object = outliner_stack_focus_object_get(ctx, focus);
    if (object == nullptr) {
      return;
    }
    /* One entry per material slot, empty slots included: the index is the slot, so skipping one
     * would shift every slot after it. */
    for (const int slot : IndexRange(object->totcol)) {
      const Material *material = BKE_object_material_get(object, short(slot + 1));
      StackSubSelection item;
      item.name = material != nullptr ? material->id.name + 2 : "";
      item.preview_id = (material != nullptr) ? &const_cast<Material *>(material)->id : nullptr;
      r_items.append(std::move(item));
    }
  }

  void sub_selection_apply(bContext & /*C*/,
                           const StackFocus &focus,
                           Object &object) const override
  {
    /* Material slots are one-based in Object::actcol, unlike the Stack API's zero-based index. */
    if (focus.sub_index >= 0 && focus.sub_index < object.totcol) {
      object.actcol = short(focus.sub_index + 1);
    }
  }

  uint64_t state_hash(const StackReadContext &ctx, const ID &owner) const override
  {
    const Material &material = paint_owner(owner);
    uint64_t topology = 0;
    if (material.nodetree != nullptr) {
      Set<const bNodeTree *> visited;
      paint_topology_hash(*material.nodetree, visited, topology);
    }
    /* Maps are found by tag as well as by link, so a tagged image appearing or disappearing
     * changes the rows without changing the graph. The revision covers everything the edit paths
     * wrote; the tag scan is what remains for the maps the paint code adds on its own.
     *
     * The tags themselves are folded in, not merely counted: a tag moved from one image to
     * another leaves the count where it was, and the rows read the tag, not the total. This is a
     * pointer walk over #Main::images reading one field each -- the cost this carries, and the
     * reason it is not a deeper read. */
    uint64_t tags = 0;
    if (ctx.bmain != nullptr) {
      for (const Image &image : ctx.bmain->images) {
        if (BLI_uuid_is_nil(image.paint_layer_id)) {
          continue;
        }
        /* A slot's blank answer reads the image's generated state and its dirty flag. The stroke
         * that first touches a blank map flips it, and has to flip this hash with it -- otherwise
         * the thumbnail would stay the placeholder until something unrelated rebuilt the rows.
         * It is mixed per image rather than accumulated on its own, so that two maps whose blank
         * answers change in opposite directions cannot cancel each other out. */
        tags = tags * 1000003u ^ uint64_t(image.id.session_uid) * 2654435761u ^
               UUID(image.paint_layer_id).hash() ^
               (uint64_t(paint_image_is_blank(image)) * 0x9E3779B97F4A7C15ull);
      }
    }
    return topology ^ tags ^ BKE_material_paint_layer_revision_get(material);
  }

  bool rows_build(const StackReadContext &ctx,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  Vector<StackRow> &r_rows) const override
  {
    if (ctx.bmain == nullptr) {
      return false;
    }
    const Material &material = paint_owner(owner);
    Vector<PaintMaterialLayerStackEntry> entries;
    if (!BKE_paint_material_layer_stack_from_material(*ctx.bmain, material, entries)) {
      return false;
    }
    /* Blending Mode and Opacity are per channel in the graph; the scene's layer-channel picker
     * says which one the rows show. A row without that channel keeps the columns empty. Layer
     * rows and correction rows read the same channel. */
    const int shown_channel = ctx.scene != nullptr && ctx.scene->toolsettings != nullptr ?
                                  ctx.scene->toolsettings->paint_mode.stack_layer_channel :
                                  int(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    /* The ordinals the layer rows below were actually emitted at: a correction hangs off its
     * parent's row, so one whose parent is missing has nothing to hang off. */
    Set<int> shown_layer_ordinals;
    bool overflow_stub_emitted = false;
    for (PaintMaterialLayerStackEntry &entry : entries) {
      if (entry.ordinal > STACK_ROW_ORDINAL_MAX) {
        /* The tree store keys the sub-rows as `ordinal * STACK_ROW_SUB_ROW_STRIDE + role`, so
         * past #STACK_ROW_ORDINAL_MAX there is no row the mode can address. The seam's contract
         * is to still show what cannot be represented: everything above the addressable range
         * collapses into one stub row with no controls, whose badge speaks for the layers it
         * stands in for. */
        StackRow row;
        row.ordinal = STACK_ROW_ORDINAL_MAX + 1;
        row.supported = false;
        row.unsupported_reason = "Stack is too large to display";
        row.name = std::move(entry.name);
        row.icon = ICON_IMAGE_RGB;
        r_rows.append(std::move(row));
        overflow_stub_emitted = true;
        break;
      }
      StackRow row;
      row.ordinal = entry.ordinal;
      row.depth = entry.depth;
      row.stable_id = entry.marker;
      /* Filled in below: the model lists a group after the layers it holds, so the enclosing row
       * is not in `r_rows` yet. */
      row.parent_ordinal = -1;
      row.can_hold_children = entry.is_group;
      row.has_children = entry.is_group;
      row.is_bare_base = entry.is_bare_base;
      row.enabled = entry.enabled;
      row.mask_enabled = entry.mask_enabled;
      row.supported = entry.supported;
      row.unsupported_reason = entry.unsupported_reason;
      row.name = std::move(entry.name);
      row.name_buffer = entry.label;
      row.color_tag = entry.color_tag;
      /* A group that stands for a material reads as that material: its icon and its preview, the
       * same way a layer reads as its map. A plain group keeps the folder. */
      Material *group_material = (entry.group_tree != nullptr) ?
                                     BKE_paint_material_layer_group_material_get(
                                         entry.group_tree->id) :
                                     nullptr;
      /* What the row *is*, read off its kind marker: a baked material reads as its material, a
       * fill as the flat colour it stands for, and a row with no marker stays a plain layer. A
       * mask keeps its icon priority over all of these. */
      const PaintMaterialLayerKind row_kind = entry.kind;
      row.icon = entry.is_group ? (group_material != nullptr ? ICON_MATERIAL : ICON_FILE_FOLDER) :
                 entry.has_mask ? (entry.mask_enabled ? ICON_MOD_MASK : ICON_MOD_SUBSURF) :
                 row_kind == PaintMaterialLayerKind::Material ? ICON_MATERIAL :
                 row_kind == PaintMaterialLayerKind::Fill ? ICON_GP_DRAW_FILL :
                                                            ICON_IMAGE_RGB;

      /* The material a Material row was baked from, resolved off its maps: the first one that
       * still carries the baked-from link speaks for the row. */
      Material *source_material = nullptr;
      if (!entry.is_group && row_kind == PaintMaterialLayerKind::Material) {
        const Image *base_map = entry.channel_images.lookup_default(
            PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr);
        ImageMaterialSource material_source;
        if (base_map != nullptr && BKE_image_material_source_get(*base_map, material_source)) {
          source_material = material_source.material;
        }
        if (source_material == nullptr) {
          for (const Image *map : entry.channel_images.values()) {
            if (map != nullptr && BKE_image_material_source_get(*map, material_source)) {
              source_material = material_source.material;
              break;
            }
          }
        }
      }

      /* A row's slots and sections. A group that stands for a material reads as that material:
       * one material slot, no content to switch. A plain group holds no map of its own -- its
       * only slot is the mask it may have. A layer gets the channels slot and section, plus the
       * mask pair when a mask exists. */
      const Image *mask_image = entry.channel_images.lookup_default(PAINT_LAYER_MAP_MASK, nullptr);
      const bool has_mask_section = mask_image != nullptr || !entry.mask_corrections.is_empty();
      if (group_material != nullptr) {
        StackRowPreview material_slot;
        material_slot.id_uid = group_material->id.session_uid;
        material_slot.id_type = ID_MA;
        row.preview_slots.append(std::move(material_slot));
      }
      else if (entry.is_group) {
        /* A group's MASK section lists its mask corrections (D15) even before it has a mask
         * image of its own: the section is what the correction rows hang under. */
        if (has_mask_section) {
          row.preview_slots.append(paint_mask_slot_build(true, entry.mask_enabled));
          row.content_sections.append(mask_image != nullptr ?
                                          paint_mask_section_build(*mask_image) :
                                          paint_mask_section_empty_build());
        }
      }
      else if (entry.supported) {
        row.preview_slots.append(paint_channels_slot_build(entry));
        if (row_kind == PaintMaterialLayerKind::Fill) {
          /* The colour the layer stands for, beside its map: the map is the fill today, but it is
           * paintable, and the swatch keeps saying what the layer was filled with. */
          StackRowPreview fill_swatch;
          fill_swatch.is_color_swatch = true;
          copy_v4_v4(fill_swatch.color, entry.fill_color);
          fill_swatch.label = IFACE_("Fill Color");
          row.preview_slots.append(std::move(fill_swatch));
        }
        if (source_material != nullptr) {
          /* A baked material row shows its source material's preview, the way a group that stands
           * for a material does. */
          StackRowPreview material_slot;
          material_slot.id_uid = source_material->id.session_uid;
          material_slot.id_type = ID_MA;
          row.preview_slots.append(std::move(material_slot));
        }
        if (has_mask_section) {
          row.preview_slots.append(paint_mask_slot_build(false, entry.mask_enabled));
        }
        row.content_sections.append(paint_channels_section_build(entry));
        if (mask_image != nullptr) {
          row.content_sections.append(paint_mask_section_build(*mask_image));
        }
        else if (!entry.mask_corrections.is_empty()) {
          /* The layer exposes its MASK section although it has no mask image (spec D15): the
           * corrections limiting where it applies are what the section lists. */
          row.content_sections.append(paint_mask_section_empty_build());
        }
      }
      if (const PointerRNA *factor = entry.channel_factor_props.lookup_ptr(shown_channel)) {
        row.value_ptr = *factor;
        /* #RNA_PaintMaterialLayerOpacity's own "value", always 0-100%; see
         * #layer_model_entry_from_node. */
        row.value_prop = "value";
      }
      if (const PointerRNA *blend = entry.channel_blend_props.lookup_ptr(shown_channel)) {
        row.mode_ptr = *blend;
        row.mode_prop = "blend_type";
      }

      shown_layer_ordinals.add(entry.ordinal);
      r_rows.append(std::move(row));
    }

    paint_correction_rows_append(
        entries, shown_layer_ordinals, overflow_stub_emitted, shown_channel, r_rows);

    /* A group is listed after the rows it holds -- that is the order the reader walks the graph in
     * -- so the enclosing row of a nested one is the first row *after* it that sits one level up.
     * Resolved here, once every row exists. */
    for (const int64_t index : r_rows.index_range()) {
      StackRow &row = r_rows[index];
      if (row.depth == 0) {
        continue;
      }
      for (int64_t next = index + 1; next < r_rows.size(); next++) {
        if (!r_rows[next].parent_section_id.empty()) {
          /* A correction row is nobody's parent: it hangs off a layer itself. */
          continue;
        }
        if (r_rows[next].depth == row.depth - 1) {
          row.parent_ordinal = r_rows[next].ordinal;
          break;
        }
      }
    }
    return !r_rows.is_empty();
  }

  bool is_editable(const ID &owner) const override
  {
    const Material &material = paint_owner(owner);
    return ID_IS_EDITABLE(&material.id) && !ID_IS_OVERRIDE_LIBRARY(&material.id) &&
           material.nodetree != nullptr && ID_IS_EDITABLE(&material.nodetree->id) &&
           !ID_IS_OVERRIDE_LIBRARY(material.nodetree);
  }

  StackColumnLayout column_layout() const override
  {
    /* Two and a half units each: the whole-percent opacity and the blend mode dropdown both fit
     * their labels without truncation, side by side or stacked in Large rows. */
    return {2.6f, 2.6f};
  }

  bool row_activate(bContext &C,
                    const StackFocus & /*focus*/,
                    ID &owner,
                    const int ordinal,
                    const StackRow &row) const override
  {
    Material &material = paint_owner(owner);
    /* The UI greys these out already, but a shortcut or a Python call reaches the operator
     * directly. */
    if (!this->is_editable(owner)) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Paint layer graph is not editable");
      return false;
    }
    if (!row.supported) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Unsupported paint layer cannot be activated");
      return false;
    }
    Scene *scene = CTX_data_scene(&C);
    if (scene == nullptr || scene->toolsettings == nullptr) {
      return false;
    }
    for (const StackContentSection &section : row.content_sections) {
      for (const StackSubRow &sub_row : section.sub_rows) {
        if (sub_row.id != nullptr && !ID_IS_EDITABLE(sub_row.id)) {
          BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Paint target image is not editable");
          return false;
        }
      }
    }

    PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
    /* Activating a row that does not own the mask being painted leaves mask editing: the
     * highlight would move on while the stroke keeps writing into the hidden mask. */
    if (paint_mode.mask_image_binding.image != nullptr &&
        !paint_row_owns_mask_image(row, paint_mode.mask_image_binding.image))
    {
      ED_paint_material_mask_edit_end_if_active(C);
    }

    /* A remembered channelless correction stops being the target the moment a row that is not it
     * activates: whatever this row ends up naming, it is not that correction. */
    BKE_paint_material_active_correction_set(nullptr, {});

    if (Main *bmain = CTX_data_main(&C)) {
      const std::optional<PaintStackRowRoute> route = paint_stack_route_get(*bmain,
                                                                            material,
                                                                            ordinal);
      if (route.has_value() && route->is_correction) {
        return this->row_correction_activate(C, *bmain, *scene, material, paint_mode, *route);
      }
    }

    /* A row that names no map -- a group -- is not something the brush can write into, and
     * clearing the bindings for it would silently take the paint target away from the user. */
    if (!paint_row_has_map(row)) {
      return true;
    }

    for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
      BKE_paint_material_channel_binding_set(paint_mode.channel_image_bindings[channel],
                                            paint_row_map_get(row, channel));
    }
    WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);

    /* Activating a layer brings up the Layer Material tab the way selecting any other Outliner
     * element brings up its own: a Material layer shows the material it was baked from, every
     * other row the material that owns the stack, where its channels are switched. */
    this->properties_sync(C, material, BKE_paint_material_active_layer_source_get(paint_mode));
    return true;
  }

  /**
   * The Layer Material tab follows an activation, the way selecting any other Outliner element
   * brings up its own: \a source when the activated row names one, the material owning the stack
   * otherwise.
   */
  void properties_sync(bContext &C, const Material &material, const Material *source) const
  {
    PointerRNA source_ptr = RNA_id_pointer_create(
        const_cast<ID *>(source != nullptr ? &source->id : &material.id));
    if (bScreen *screen = CTX_wm_screen(&C)) {
      for (ScrArea &area : screen->areabase) {
        if (area.spacetype != SPACE_PROPERTIES) {
          continue;
        }
        SpaceProperties *sbuts = static_cast<SpaceProperties *>(area.spacedata.first);
        if (ED_buttons_should_sync_with_outliner(&C, sbuts, &area)) {
          ED_buttons_set_context(&C, sbuts, &source_ptr, BCONTEXT_LAYER_MATERIAL);
          ED_area_tag_redraw(&area);
        }
      }
    }
  }

  /**
   * The correction half of #row_activate (spec D16): the brush moves onto the correction's own
   * maps. A mask correction edits its mask with the mask brush; a content correction paints the
   * channels it has maps for, and one with none is remembered by identity alone, since bindings
   * cannot name it.
   */
  bool row_correction_activate(bContext &C,
                               Main &bmain,
                               Scene &scene,
                               Material &material,
                               PaintModeSettings &paint_mode,
                               const PaintStackRowRoute &route) const
  {
    Vector<PaintMaterialLayerStackEntry> entries;
    if (!BKE_paint_material_layer_stack_from_material(bmain, material, entries)) {
      return false;
    }
    const PaintMaterialLayerCorrectionEntry *correction = nullptr;
    for (const PaintMaterialLayerStackEntry &entry : entries) {
      if (entry.ordinal != route.layer_ordinal) {
        continue;
      }
      for (const Vector<PaintMaterialLayerCorrectionEntry> *corrections :
           {&entry.content_corrections, &entry.mask_corrections})
      {
        for (const PaintMaterialLayerCorrectionEntry &corr : *corrections) {
          if (BLI_uuid_equal(corr.marker, route.correction)) {
            correction = &corr;
            break;
          }
        }
        if (correction != nullptr) {
          break;
        }
      }
      if (correction != nullptr) {
        break;
      }
    }
    if (correction == nullptr) {
      return false;
    }
    if (correction->effect != PaintMaterialCorrectionEffect::Paint) {
      /* Fill corrections remain selectable rows, but cannot become a brush target. The shared
       * cleanup in #row_activate has already ended mask editing and cleared the correction memo. */
      for (MaterialPaintChannelImageBinding &binding : paint_mode.channel_image_bindings) {
        BKE_paint_material_channel_binding_set(binding, nullptr);
      }
      WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
      this->properties_sync(C, material, nullptr);
      return true;
    }

    if (route.section == PaintMaterialCorrectionSection::Mask) {
      /* Same form as #paint_material_mask_preview_activate: the correction's own map becomes the
       * mask being edited, replacing whatever mask was. A correction with no map there yet has
       * nothing to switch the mask brush onto -- and mask editing cannot survive, since the row
       * that owned it just stopped being shown as editable. */
      if (Image *mask_image = correction->channel_images.lookup_default(PAINT_LAYER_MAP_MASK,
                                                                        nullptr);
          mask_image != nullptr)
      {
        Paint *paint = BKE_paint_get_active_from_context(&C);
        if (paint == nullptr) {
          return false;
        }
        BKE_paint_material_mask_edit_begin_ex(bmain, scene, *paint, paint_mode, *mask_image);
        /* The mask brush was just swapped in, the way a preview-slot click tells the brush UI. */
        WM_event_add_notifier(&C, NC_BRUSH | NA_EDITED, nullptr);
      }
      else if (paint_mode.mask_image_binding.image != nullptr) {
        ED_paint_material_mask_edit_end_if_active(C);
      }
      WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
      this->properties_sync(C, material, nullptr);
      return true;
    }

    /* A content correction paints the channels its maps are on and unbinds the rest: a channel
     * without a map of the correction's own stays empty rather than writing into the layer
     * underneath. */
    ED_paint_material_mask_edit_end_if_active(C);
    bool any_map = false;
    for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
      Image *map = correction->channel_images.lookup_default(channel, nullptr);
      any_map = any_map || map != nullptr;
      BKE_paint_material_channel_binding_set(paint_mode.channel_image_bindings[channel], map);
    }
    /* The maps name the row by themselves; only a correction without any is remembered, for
     * #BKE_paint_material_active_layer_get to still answer with once the bindings are all null. */
    if (any_map) {
      BKE_paint_material_active_correction_set(nullptr, {});
    }
    else {
      BKE_paint_material_active_correction_set(&material, route.correction);
    }
    WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
    this->properties_sync(C, material, nullptr);
    return true;
  }

  bool row_is_active(const StackReadContext &ctx,
                     const StackFocus & /*focus*/,
                     const ID &owner,
                     const StackRow &row) const override
  {
    if (!row.supported || ctx.bmain == nullptr || ctx.scene == nullptr ||
        ctx.scene->toolsettings == nullptr)
    {
      return false;
    }
    if (const std::optional<PaintMaterialCorrectionEffect> effect =
            paint_correction_effect_get(*ctx.bmain, paint_owner(owner), row.stable_id);
        effect.has_value() && *effect != PaintMaterialCorrectionEffect::Paint)
    {
      return false;
    }
    /* The shared resolver names a layer row by (owner, ordinal), which a map-less row -- a group
     * -- can never be: it holds no channel map of its own for the resolver to match a binding
     * against, so it is never the row a search over #bmain.materials lands on. No separate check
     * is needed for that case here. */
    const std::optional<PaintMaterialActiveLayer> active = BKE_paint_material_active_layer_get(
        *ctx.bmain, ctx.scene->toolsettings->paint_mode);
    if (!active.has_value() || active->owner != &paint_owner(owner)) {
      return false;
    }
    /* A correction row is named by its marker, not by an ordinal: the resolver answers with the
     * correction's owning layer's ordinal, and the row's own budget lives above the layers'.
     * A nil answer correction is a layer row, matched the old way. */
    if (!BLI_uuid_is_nil(active->correction)) {
      return BLI_uuid_equal(active->correction, row.stable_id);
    }
    return active->ordinal == row.ordinal;
  }

  /**
   * The shape every mutating method below shares: get the material, call one
   * `BKE_paint_material_layer_*` function with the shared error out-param, report on failure,
   * notify the scene on success.
   *
   * \param fn: `bool(Main &, Material &, PaintMaterialLayerEditError &)` -- the one call that
   * differs between callers. A caller returning an ordinal captures its own `int` and reads it
   * back after this returns; it is left at its "nothing happened" value on failure either way.
   */
  template<typename Fn> bool paint_edit(bContext &C, ID &owner, Fn &&fn) const
  {
    Material &material = paint_owner(owner);
    Main &bmain = *CTX_data_main(&C);
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    if (!fn(bmain, material, error)) {
      BKE_report(
          CTX_wm_reports(&C), RPT_ERROR, RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  /**
   * The quiet shape of #paint_edit for per-tick picker previews: the same BKE call and the same
   * notifier on success, but a refusal stays silent -- a picker tick must never spam the status
   * bar -- and no undo step is involved either way (the caller is an RNA update, not an exec).
   */
  template<typename Fn> bool paint_preview(bContext &C, ID &owner, Fn &&fn) const
  {
    Material &material = paint_owner(owner);
    Main &bmain = *CTX_data_main(&C);
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    if (!fn(bmain, material, error)) {
      return false;
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  /**
   * The route \a ordinal names when it names a correction row, against the model as it stands
   * now. Every mutating method asks this first and hands the correction to the correction API;
   * a layer row (or an ordinal naming nothing) falls through to the layer path.
   */
  std::optional<PaintStackRowRoute> correction_route_get(bContext &C,
                                                         ID &owner,
                                                         const int ordinal) const
  {
    Main *bmain = CTX_data_main(&C);
    if (bmain == nullptr) {
      return std::nullopt;
    }
    const std::optional<PaintStackRowRoute> route = paint_stack_route_get(
        *bmain, paint_owner(owner), ordinal);
    if (route.has_value() && route->is_correction) {
      return route;
    }
    return std::nullopt;
  }

  /**
   * Capture the layer's pristine pixels into the picker's session tile map, before the first
   * byte a preview tick or a bake writes.
   *
   * First touch wins: #ED_image_paint_tile_push captures a tile only once, so calling this on
   * every tick keeps the true pre-picker canvas while staying a hash lookup after the first
   * pass. Best effort -- a layer whose rows cannot be resolved (a mid-dialog race) simply
   * captures nothing, and the fill proceeds without a safety net for those maps.
   */
  static void paint_fill_session_ensure(Main &bmain,
                                       Material &material,
                                       const int ordinal,
                                       PaintTileMap &session_tiles)
  {
    Vector<PaintMaterialLayerStackEntry> entries;
    if (!BKE_paint_material_layer_stack_from_material(bmain, material, entries)) {
      return;
    }
    const PaintMaterialLayerStackEntry *target = nullptr;
    for (const PaintMaterialLayerStackEntry &entry : entries) {
      if (entry.ordinal == ordinal) {
        target = &entry;
        break;
      }
    }
    if (target == nullptr) {
      return;
    }
    for (Image *image : target->channel_images.values()) {
      if (image == nullptr) {
        continue;
      }
      for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
        ImageUser iuser;
        BKE_imageuser_default(&iuser);
        iuser.tile = tile->tile_number;
        void *lock = nullptr;
        ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
        if (ibuf == nullptr) {
          continue;
        }
        const int tiles_x = ED_IMAGE_UNDO_TILE_NUMBER(ibuf->x);
        const int tiles_y = ED_IMAGE_UNDO_TILE_NUMBER(ibuf->y);
        for (int ty = 0; ty < tiles_y; ty++) {
          for (int tx = 0; tx < tiles_x; tx++) {
            ED_image_paint_tile_push(
                &session_tiles, image, ibuf, &iuser, tx, ty, nullptr, nullptr, false, true);
          }
        }
        BKE_image_release_ibuf(image, ibuf, lock);
      }
    }
  }

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
    const std::optional<PaintStackRowRoute> from_correction = this->correction_route_get(
        C, owner, from_ordinal);
    if (from_correction.has_value()) {
      const std::optional<PaintStackRowRoute> to_correction = this->correction_route_get(
          C, owner, to_ordinal);
      /* Two corrections trade places only within one section of one layer; anything else is a
       * move the correction API does not express. */
      if (!to_correction.has_value() ||
          to_correction->layer_ordinal != from_correction->layer_ordinal ||
          to_correction->section != from_correction->section)
      {
        return false;
      }
      const bUUID moved = from_correction->correction;
      const bUUID target = to_correction->correction;
      return this->paint_edit(
          C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
            /* #BKE_paint_material_layer_correction_reorder names the destination by its index in
             * the section, which is the position the row it lands at -- the one \a to_ordinal
             * names -- has right now. */
            PaintMaterialCorrectionSection to_section = PaintMaterialCorrectionSection::Content;
            int to_index = -1;
            if (BKE_paint_material_layer_correction_owner_ordinal(
                    bmain, material, target, &to_section, &to_index) < 0 ||
                to_index < 0)
            {
              error = PaintMaterialLayerEditError::CorrectionNotFound;
              return false;
            }
            return BKE_paint_material_layer_correction_reorder(
                bmain, material, moved, to_index, &error);
          });
    }
    return this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_reorder(
              bmain, material, from_ordinal, to_ordinal, &error);
        });
  }

  bool row_move(bContext &C,
                const StackFocus & /*focus*/,
                ID &owner,
                const int from_ordinal,
                const int anchor_ordinal,
                const StackMovePlace place,
                int *r_ordinal) const override
  {
    const std::optional<PaintStackRowRoute> from_correction = this->correction_route_get(
        C, owner, from_ordinal);
    if (from_correction.has_value()) {
      /* A drop beside a correction row reorders within the section it hangs under: only
       * Above/Below of an anchor correction in that same section is a place the correction API
       * names, and no reading of Into exists for a row that holds nothing. */
      if (place == StackMovePlace::Into) {
        return false;
      }
      const std::optional<PaintStackRowRoute> anchor_correction = this->correction_route_get(
          C, owner, anchor_ordinal);
      if (!anchor_correction.has_value() ||
          anchor_correction->layer_ordinal != from_correction->layer_ordinal ||
          anchor_correction->section != from_correction->section)
      {
        return false;
      }
      const bUUID moved = from_correction->correction;
      const bUUID anchor_marker = anchor_correction->correction;
      const bool anchor_above = (place == StackMovePlace::Above);
      const bool moved_ok = this->paint_edit(
          C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
            PaintMaterialCorrectionSection anchor_section =
                PaintMaterialCorrectionSection::Content;
            int anchor_index = -1;
            if (BKE_paint_material_layer_correction_owner_ordinal(
                    bmain, material, anchor_marker, &anchor_section, &anchor_index) < 0 ||
                anchor_index < 0)
            {
              error = PaintMaterialLayerEditError::CorrectionNotFound;
              return false;
            }
            return BKE_paint_material_layer_correction_reorder(
                bmain, material, moved, anchor_above ? anchor_index : anchor_index + 1, &error);
          });
      if (moved_ok && r_ordinal != nullptr) {
        /* The destination is the anchor's old address only when it moved above it; below it takes
         * the next one down, so the moved row's ordinal comes off a fresh read. */
        Main *bmain = CTX_data_main(&C);
        *r_ordinal = (bmain != nullptr) ?
                         paint_correction_route_ordinal_get(*bmain, paint_owner(owner), moved) :
                         -1;
      }
      return moved_ok;
    }
    /* The seam speaks of rows, this file speaks of layers; the two vocabularies meet here. */
    PaintMaterialLayerMovePlace layer_place = PaintMaterialLayerMovePlace::Above;
    switch (place) {
      case StackMovePlace::Above:
        layer_place = PaintMaterialLayerMovePlace::Above;
        break;
      case StackMovePlace::Below:
        layer_place = PaintMaterialLayerMovePlace::Below;
        break;
      case StackMovePlace::Into:
        layer_place = PaintMaterialLayerMovePlace::Into;
        break;
    }
    return this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_move(
              bmain, material, from_ordinal, anchor_ordinal, layer_place, r_ordinal, &error);
        });
  }

  void add_kinds(Vector<StackAddKindInfo> &r_kinds) const override
  {
    r_kinds.append({"PAINT",
                    "Paint Layer",
                    "A layer that shows nothing until it is painted on",
                    ICON_BRUSH_DATA});
    StackAddKindInfo fill{"FILL",
                          "Fill Layer",
                          "A layer of one flat colour, revealed by its mask",
                          ICON_GP_DRAW_FILL};
    fill.takes_color = true;
    r_kinds.append(fill);
    /* Made from a material the user picks: the ID browser chooses it and hands its name to the
     * Add, so the kind is declared -- scripts and repeats can name it -- but no UI offers it as a
     * plain Add button. */
    StackAddKindInfo material{"MATERIAL",
                              "Material Layer",
                              "A material from the file or an asset library, baked into the "
                              "layer's maps",
                              ICON_MATERIAL};
    material.source_id_type = ID_MA;
    r_kinds.append(material);
    /* A correction hangs off the row the Add was anchored on, so these kinds mean nothing without
     * an anchor; #row_add refuses when the anchor names no row. Which section the correction
     * lands in follows the anchor -- see #row_add. */
    r_kinds.append({"CORRECTION",
                    IFACE_("Correction"),
                    "An adjustment row hung on the content of the row this is added from",
                    ICON_BRUSH_DATA});
    r_kinds.append({"MASK_CORRECTION",
                    IFACE_("Mask Correction"),
                    "A correction limiting where the row this is added from applies",
                    ICON_BRUSH_DATA});
  }

  /**
   * Add a layer whose maps are a picked material baked per channel.
   *
   * A thin call into #ed::sculpt_paint::material_layer::add_from_material, which owns the
   * "bake -> hand the targets over -> stamp Material" sequence shared with the Layer Material
   * tab's own operators; the Outliner only resolves \a owner to the #Material it names.
   */
  int row_add_material(bContext &C, ID &owner, const int ordinal, Material &source) const
  {
    return ed::sculpt_paint::material_layer::add_from_material(
        C, paint_owner(owner), ordinal, source);
  }

  int row_add(bContext &C,
              const StackFocus & /*focus*/,
              ID &owner,
              const int kind,
              const int ordinal,
              const StackAddArgs &args = {}) const override
  {
    /* The kinds this editor declares, by their place in #add_kinds' list. */
    const int fill_kind = 1;
    const int material_kind = 2;
    const int correction_kind = 3;
    const int mask_correction_kind = 4;
    if (kind == material_kind) {
      /* The Add resolved the source by the ID type this kind declared; anything else is a caller
       * that went around it. */
      if (args.source == nullptr || GS(args.source->name) != ID_MA) {
        return -1;
      }
      return this->row_add_material(C, owner, ordinal, *id_cast<Material *>(args.source));
    }
    if (ELEM(kind, correction_kind, mask_correction_kind)) {
      /* A correction hangs off the layer the anchor row stands for -- the anchor row itself when
       * it is a layer, its parent when it is another correction. An Add with no anchor names no
       * parent, and inventing one would put the correction somewhere the user did not ask for. */
      Main *bmain = CTX_data_main(&C);
      if (bmain == nullptr) {
        return -1;
      }
      const std::optional<PaintStackRowRoute> anchor = paint_stack_route_get(
          *bmain, paint_owner(owner), ordinal);
      if (!anchor.has_value()) {
        return -1;
      }
      PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
      if (kind == mask_correction_kind ||
          (anchor->is_correction &&
           anchor->section == PaintMaterialCorrectionSection::Mask) ||
          args.section_id == "MASK")
      {
        section = PaintMaterialCorrectionSection::Mask;
      }
      const int layer_ordinal = anchor->layer_ordinal;
      /* The Add's `effect` property, numbered as this enum is; anything else reads as the painted
       * adjustment the Add has always created. */
      const PaintMaterialCorrectionEffect effect =
          args.effect == int(PaintMaterialCorrectionEffect::Fill) ?
              PaintMaterialCorrectionEffect::Fill :
              PaintMaterialCorrectionEffect::Paint;
      bUUID marker = {};
      const bool added = this->paint_edit(
          C,
          owner,
          [&](Main &route_bmain, Material &material, PaintMaterialLayerEditError &error) {
            return BKE_paint_material_layer_correction_add(
                route_bmain, material, layer_ordinal, section, effect, nullptr, &marker, &error);
          });
      if (!added) {
        return -1;
      }
      /* The new row's address comes off a fresh read: the budget allocated by the model the edit
       * just changed. */
      return paint_correction_route_ordinal_get(*bmain, paint_owner(owner), marker);
    }
    PaintMaterialLayerAddParams params;
    params.kind = (kind == fill_kind) ? PaintMaterialLayerKind::Fill :
                                        PaintMaterialLayerKind::Paint;
    if (kind == fill_kind && args.color != nullptr) {
      copy_v4_v4(params.fill_color, args.color);
    }
    /* The seam hands #row_add the row the Add was invoked on, or -1 for the top of the stack. A
     * row inside a folder -- or a folder itself -- is placed relative to through
     * #PaintMaterialLayerAddParams::anchor_ordinal, which walks into that folder's own node
     * tree; -1 stays the plain "on top of the top-level stack". */
    params.anchor_ordinal = ordinal;
    const Scene *scene = CTX_data_scene(&C);
    if (scene != nullptr && scene->toolsettings != nullptr) {
      /* The same size the first brush stroke would have created this material's maps at. */
      params.image_size = scene->toolsettings->paint_mode.new_channel_image_size;
    }
    int new_ordinal = -1;
    this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_add(bmain, material, params, &new_ordinal, &error);
        });
    return new_ordinal;
  }

  bool row_set_enabled(bContext &C,
                       const StackFocus & /*focus*/,
                       ID &owner,
                       const int ordinal,
                       const bool enable) const override
  {
    if (const std::optional<PaintStackRowRoute> correction = this->correction_route_get(
            C, owner, ordinal))
    {
      return this->paint_edit(
          C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
            return BKE_paint_material_layer_correction_set_enabled(
                bmain, material, correction->correction, enable, &error);
          });
    }
    return this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_set_enabled(bmain, material, ordinal, enable, &error);
        });
  }

  int row_duplicate(bContext &C,
                    const StackFocus & /*focus*/,
                    ID &owner,
                    const int ordinal) const override
  {
    if (const std::optional<PaintStackRowRoute> correction = this->correction_route_get(
            C, owner, ordinal))
    {
      const int layer_ordinal = correction->layer_ordinal;
      Vector<bUUID> created;
      PaintMaterialCorrectionCopyReport copy_report;
      const bool copied = this->paint_edit(
          C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
            /* One ref of this material's own correction onto the same layer: the copy API is the
             * cross-material one, and a duplicate is the within-one-material reading of it. */
            const PaintMaterialCorrectionRef source_ref{material.id.session_uid,
                                                        correction->correction};
            return BKE_paint_material_layer_corrections_copy(
                bmain, {source_ref}, material, layer_ordinal, created, &copy_report, &error);
          });
      if (!copied || created.is_empty()) {
        return -1;
      }
      /* The copy's address comes off a fresh read: the budget allocated by the model the edit
       * just changed, and the copy is a new correction in it. */
      Main *bmain = CTX_data_main(&C);
      return (bmain != nullptr) ?
                 paint_correction_route_ordinal_get(*bmain, paint_owner(owner), created[0]) :
                 -1;
    }
    int new_ordinal = -1;
    this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_duplicate(
              bmain, material, ordinal, &new_ordinal, &error);
        });
    return new_ordinal;
  }

  bool can_paste_into(const StackReadContext &ctx,
                      const StackFocus & /*focus*/,
                      const ID &owner,
                      const StackItemIdentity &source,
                      const int target_ordinal) const override
  {
    if (ctx.bmain == nullptr || source.source_type != this->type() || !source.is_valid() ||
        BLI_uuid_is_nil(source.row_id))
    {
      return false;
    }
    Material *source_material = id_cast<Material *>(
        BKE_libblock_find_session_uid(ctx.bmain, ID_MA, source.owner_uid));
    if (source_material == nullptr) {
      return false;
    }
    /* The source side: the identity has to name a correction this source's model still has. The
     * section it hangs under decides what it can be pasted onto. */
    PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
    if (BKE_paint_material_layer_correction_owner_ordinal(
            *ctx.bmain, *source_material, source.row_id, &section, nullptr) < 0)
    {
      return false;
    }
    /* The target side: a layer takes corrections in either section; a group keeps its content
     * inside its folder, so only a mask correction -- one limiting where the group's result
     * applies -- is a paste a group row can take. */
    Vector<PaintMaterialLayerStackEntry> target_entries;
    if (!BKE_paint_material_layer_stack_from_material(*ctx.bmain,
                                                      paint_owner(owner),
                                                      target_entries))
    {
      return false;
    }
    for (const PaintMaterialLayerStackEntry &entry : target_entries) {
      if (entry.ordinal == target_ordinal) {
        return !entry.is_group || section == PaintMaterialCorrectionSection::Mask;
      }
    }
    return false;
  }

  bool rows_paste_into(bContext &C,
                       const StackFocus & /*focus*/,
                       ID &owner,
                       const Span<StackItemIdentity> sources,
                       const int target_ordinal,
                       Vector<StackItemIdentity> &r_created,
                       ReportList *reports) const override
  {
    Main *bmain = CTX_data_main(&C);
    if (bmain == nullptr) {
      return false;
    }
    Material &target = paint_owner(owner);
    /* Only this source's rows name a correction of ours; the rest of a mixed clipboard is left
     * out rather than taken down with the copies it cannot name. */
    Vector<PaintMaterialCorrectionRef> refs;
    for (const StackItemIdentity &source : sources) {
      if (source.source_type != this->type() || !source.is_valid() ||
          BLI_uuid_is_nil(source.row_id))
      {
        continue;
      }
      refs.append({source.owner_uid, source.row_id});
    }
    if (refs.is_empty()) {
      return false;
    }

    /* The target row's name, read before the copy: the reports below name the layer the channels
     * were wired on. */
    std::string target_name;
    Vector<PaintMaterialLayerStackEntry> target_entries;
    if (BKE_paint_material_layer_stack_from_material(*bmain, target, target_entries)) {
      for (const PaintMaterialLayerStackEntry &entry : target_entries) {
        if (entry.ordinal == target_ordinal) {
          target_name = entry.name;
          break;
        }
      }
    }

    Vector<bUUID> created_markers;
    PaintMaterialCorrectionCopyReport copy_report;
    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    const bool copied = BKE_paint_material_layer_corrections_copy(*bmain,
                                                                  refs,
                                                                  target,
                                                                  target_ordinal,
                                                                  created_markers,
                                                                  &copy_report,
                                                                  &error);
    if (!copied) {
      /* A refusal can still have pasted some of the copies -- a mid-copy failure leaves what it
       * wrote for the caller's undo step to take back or keep -- so what landed is reported all
       * the same, as a warning rather than an error. */
      BKE_reportf(reports,
                  RPT_WARNING,
                  RPT_("Could not copy all corrections: %s"),
                  RPT_(BKE_paint_material_layer_edit_error_message(error)));
    }
    for (const int channel : copy_report.enabled_parent_channels) {
      const char *channel_name = IFACE_("Channel");
      RNA_enum_name_gettexted(rna_enum_material_paint_channel_items,
                              channel,
                              BLT_I18NCONTEXT_DEFAULT,
                              &channel_name);
      BKE_reportf(reports,
                  RPT_INFO,
                  RPT_("Enabled channel %s on \"%s\""),
                  channel_name,
                  target_name.c_str());
    }
    if (copy_report.scaled_maps > 0) {
      int width = 0;
      int height = 0;
      if (BKE_paint_material_layer_map_size_get(*bmain, target, target_ordinal, width, height)) {
        BKE_reportf(reports,
                    RPT_INFO,
                    RPT_("Scaled %d map(s) to %dx%d"),
                    copy_report.scaled_maps,
                    width,
                    height);
      }
      else {
        BKE_reportf(reports, RPT_INFO, RPT_("Scaled %d map(s)"), copy_report.scaled_maps);
      }
    }
    const int skipped = copy_report.skipped_missing + copy_report.skipped_group;
    if (skipped > 0) {
      BKE_reportf(reports, RPT_INFO, RPT_("Skipped %d correction(s)"), skipped);
    }

    for (const bUUID &marker : created_markers) {
      StackItemIdentity identity;
      identity.owner_uid = target.id.session_uid;
      identity.source_type = this->type();
      identity.row_id = marker;
      r_created.append(identity);
    }
    if (!created_markers.is_empty()) {
      WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &target.id);
    }
    return !created_markers.is_empty();
  }

  bool row_rename(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal,
                  const StringRefNull name) const override
  {
    if (const std::optional<PaintStackRowRoute> correction = this->correction_route_get(
            C, owner, ordinal))
    {
      return this->paint_edit(
          C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
            return BKE_paint_material_layer_correction_rename(
                bmain, material, correction->correction, name, &error);
          });
    }
    return this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_rename(bmain, material, ordinal, name.c_str(), &error);
        });
  }

  bool row_mask_set(bContext &C,
                    const StackFocus & /*focus*/,
                    ID &owner,
                    const int ordinal,
                    const bool add,
                    const float initial_color[4]) const override
  {
    if (this->correction_route_get(C, owner, ordinal).has_value()) {
      /* A correction has no mask image of its own to add or take away -- the MASK section it
       * hangs under is already the mask half of its parent. */
      return false;
    }
    int image_size = 1024;
    if (add) {
      const Scene *scene = CTX_data_scene(&C);
      if (scene != nullptr && scene->toolsettings != nullptr) {
        image_size = scene->toolsettings->paint_mode.new_channel_image_size;
      }
    }
    Material &owner_material = paint_owner(owner);
    Scene *scene = CTX_data_scene(&C);
    if (!add && scene != nullptr && scene->toolsettings != nullptr) {
      const Image *edited = scene->toolsettings->paint_mode.mask_image_binding.image;
      if (edited != nullptr &&
          edited == paint_row_mask_image_get(*CTX_data_main(&C), owner_material, ordinal))
      {
        /* Leave mask editing before the image is freed: the remap would only null the target and
         * leave the mask brush active instead of restoring the one the channels were painted
         * with. */
        ED_paint_material_mask_edit_end_if_active(C);
      }
    }
    const bool changed = this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return add ? BKE_paint_material_layer_mask_add(
                           bmain, material, ordinal, initial_color, image_size, &error) :
                       BKE_paint_material_layer_mask_remove(bmain, material, ordinal, &error);
        });
    if (changed && add) {
      paint_mask_edit_begin_for_new_mask(C, owner, owner_material, ordinal);
    }
    return changed;
  }

  bool row_mask_toggle(bContext &C,
                       const StackFocus & /*focus*/,
                       ID &owner,
                       const int ordinal) const override
  {
    if (this->correction_route_get(C, owner, ordinal).has_value()) {
      /* A correction's MASK section is its parent's mask half, not a mask of its own. */
      return false;
    }
    Material &owner_material = paint_owner(owner);
    Main *bmain = CTX_data_main(&C);
    if (bmain == nullptr) {
      return false;
    }
    /* The toggle flips whatever the row's mask is now; the image also carries the state, so the
     * read and the write cannot disagree. */
    Image *mask_image = paint_row_mask_image_get(*bmain, owner_material, ordinal);
    if (mask_image == nullptr) {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, RPT_("The layer has no mask"));
      return false;
    }
    const bool enable = mask_image->paint_layer_mask_disabled != 0;
    if (!enable) {
      /* Turning the mask back on is safe while editing it; turning it off takes the coverage the
       * strokes were shaping away, so the editing session ends first, the same as a remove. */
      Scene *scene = CTX_data_scene(&C);
      if (scene != nullptr && scene->toolsettings != nullptr) {
        const Image *edited = scene->toolsettings->paint_mode.mask_image_binding.image;
        if (edited == mask_image) {
          ED_paint_material_mask_edit_end_if_active(C);
        }
      }
    }
    return this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_mask_set_enabled(
              bmain, material, ordinal, enable, &error);
        });
  }

  /** The mask Image of the layer at \a ordinal, or null when it has none. */
  static Image *paint_row_mask_image_get(Main &bmain, Material &material, const int ordinal)
  {
    Vector<PaintMaterialLayerStackEntry> entries;
    if (!BKE_paint_material_layer_stack_from_material(bmain, material, entries)) {
      return nullptr;
    }
    for (const PaintMaterialLayerStackEntry &entry : entries) {
      if (entry.ordinal == ordinal) {
        return entry.channel_images.lookup_default(PAINT_LAYER_MAP_MASK, nullptr);
      }
    }
    return nullptr;
  }

  /**
   * Whether removing the row at \a ordinal also removes the mask being painted: the row itself
   * owns it, or the row is a group with the owning row somewhere under it. Found by walking the
   * owning row's parent chain up to the top level.
   */
  static bool paint_remove_drops_edited_mask(const SpaceOutliner &space_outliner,
                                             const int ordinal,
                                             const Image *edited_mask)
  {
    const StackRow *owner_row = nullptr;
    for (const StackRow &row : space_outliner.runtime->stack_rows) {
      if (paint_row_owns_mask_image(row, edited_mask)) {
        owner_row = &row;
        break;
      }
    }
    if (owner_row == nullptr) {
      return false;
    }
    for (const StackRow *row = owner_row; row != nullptr;
         row = (row->parent_ordinal < 0) ?
                   nullptr :
                   outliner_stack_row_find(space_outliner, row->parent_ordinal))
    {
      if (row->ordinal == ordinal) {
        return true;
      }
    }
    return false;
  }

  /**
   * A mask is added to be painted: make it the stroke target right away and show its content, the
   * same state a click on its preview slot leads to (see #preview_activate).
   */
  static void paint_mask_edit_begin_for_new_mask(bContext &C,
                                                 ID &owner,
                                                 Material &material,
                                                 const int ordinal)
  {
    Image *mask_image = paint_row_mask_image_get(*CTX_data_main(&C), material, ordinal);
    if (mask_image == nullptr) {
      return;
    }
    ED_paint_material_mask_edit_begin(C, *mask_image);
    WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
    WM_event_add_notifier(&C, NC_BRUSH | NA_EDITED, nullptr);

    SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
    if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
      return;
    }
    /* The rows still describe the graph before the mask existed; its state hash changed, so this
     * rebuilds them and the row has its "MASK" section to switch to. The UI state set here is
     * captured by the invalidation #stack_mutate does after this returns. */
    const StackReadContext ctx = outliner_stack_read_context(C);
    outliner_stack_rows_ensure(ctx, *space_outliner, owner);
    if (const StackRow *row = outliner_stack_row_find(*space_outliner, ordinal)) {
      outliner_stack_row_active_section_set(*space_outliner, *row, "MASK");
    }
  }

  bool row_fill_color_set(bContext &C,
                          const StackFocus & /*focus*/,
                          ID &owner,
                          const int ordinal,
                          const float color[4],
                          StackColorSession *session) const override
  {
    if (this->correction_route_get(C, owner, ordinal).has_value()) {
      return false;
    }
    if (session != nullptr && session->tiles != nullptr) {
      Material &target_material = paint_owner(owner);
      Main &bmain = *CTX_data_main(&C);
      paint_fill_session_ensure(bmain, target_material, ordinal, *session->tiles);
    }
    return this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_fill_color_apply(
              bmain, material, ordinal, color, &error);
        });
  }

  bool row_fill_color_preview(bContext &C,
                              const StackFocus & /*focus*/,
                              ID &owner,
                              const int ordinal,
                              const float color[4],
                              StackColorSession *session) const override
  {
    if (this->correction_route_get(C, owner, ordinal).has_value()) {
      return false;
    }
    if (session != nullptr && session->tiles != nullptr) {
      Material &target_material = paint_owner(owner);
      Main &bmain = *CTX_data_main(&C);
      paint_fill_session_ensure(bmain, target_material, ordinal, *session->tiles);
    }
    return this->paint_preview(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_fill_color_preview(
              bmain, material, ordinal, color, &error);
        });
  }

  StackColorSession *color_session_new() const override
  {
    StackColorSession *session = new StackColorSession();
    session->tiles = ED_image_paint_tile_map_new();
    return session;
  }

  void color_session_restore(StackColorSession &session) const override
  {
    if (session.tiles != nullptr) {
      ED_image_paint_tile_map_restore(session.tiles);
    }
  }

  void color_session_free(StackColorSession *session) const override
  {
    if (session == nullptr) {
      return;
    }
    if (session->tiles != nullptr) {
      ED_image_paint_tile_map_free(session->tiles);
    }
    delete session;
  }

  void color_session_push_undo(StackColorSession &session,
                               const StringRefNull undo_name) const override
  {
    if (session.tiles != nullptr) {
      ED_image_undo_push_from_tile_map(undo_name.c_str(), PaintMode::Texture2D, session.tiles);
    }
  }

  int rows_group(bContext &C,
                 const StackFocus & /*focus*/,
                 ID &owner,
                 const int from_ordinal,
                 const int to_ordinal) const override
  {
    if (this->correction_route_get(C, owner, from_ordinal).has_value() ||
        this->correction_route_get(C, owner, to_ordinal).has_value())
    {
      /* A correction is not something a folder can be made out of, nor a folder to make. */
      return -1;
    }
    int group_ordinal = -1;
    this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_group_make(
              bmain, material, from_ordinal, to_ordinal, &group_ordinal, &error);
        });
    return group_ordinal;
  }

  int row_merge_down(bContext &C,
                     const StackFocus & /*focus*/,
                     ID &owner,
                     const int ordinal) const override
  {
    if (this->correction_route_get(C, owner, ordinal).has_value()) {
      /* A correction has no row of its own below it to merge into: it hangs off its parent's
       * content, and the pair already reads as one. */
      return -1;
    }
    int group_ordinal = -1;
    this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          /* The pair has to be two plain sibling rows: a folder is merged as a folder or not at
           * all, rows of different folders do not cross the boundary, and the stack's own bare
           * base cannot start the range a group is built from. */
          Vector<PaintMaterialLayerStackEntry> entries;
          if (!BKE_paint_material_layer_stack_from_material(bmain, material, entries)) {
            error = PaintMaterialLayerEditError::NotAStack;
            return false;
          }
          const PaintMaterialLayerStackEntry *upper = nullptr;
          const PaintMaterialLayerStackEntry *lower = nullptr;
          for (const PaintMaterialLayerStackEntry &entry : entries) {
            if (entry.ordinal == ordinal) {
              upper = &entry;
            }
            else if (entry.ordinal == ordinal - 1) {
              lower = &entry;
            }
          }
          if (upper == nullptr || lower == nullptr || upper->depth != lower->depth ||
              upper->is_group || lower->is_group || lower->is_bare_base)
          {
            error = PaintMaterialLayerEditError::NotAStack;
            return false;
          }
          return BKE_paint_material_layer_group_make(
              bmain, material, ordinal - 1, ordinal, &group_ordinal, &error);
        });
    return group_ordinal;
  }

  int group_add(bContext &C,
                const StackFocus & /*focus*/,
                ID &owner,
                const int ordinal) const override
  {
    int group_ordinal = -1;
    this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_group_add(bmain,
                                                    material,
                                                    ordinal,
                                                    PaintMaterialLayerMovePlace::Above,
                                                    &group_ordinal,
                                                    &error);
        });
    return group_ordinal;
  }

  int row_ungroup(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal) const override
  {
    int layer_num = -1;
    this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_group_ungroup(
              bmain, material, ordinal, &layer_num, &error);
        });
    return layer_num;
  }

  bool row_color_tag_set(bContext &C,
                         const StackFocus & /*focus*/,
                         ID &owner,
                         const int ordinal,
                         const int color_tag) const override
  {
    if (this->correction_route_get(C, owner, ordinal).has_value()) {
      /* Color tags are a group-row affordance; a correction is not a folder. */
      return false;
    }
    /* Color tags are stored on the group node, not in the layer stack itself, so this does not go
     * through paint_edit (which triggers stack mutations). It is a direct node property change. */
    Material &material = paint_owner(owner);
    Main *bmain = CTX_data_main(&C);
    Vector<PaintMaterialLayerStackEntry> entries;
    if (!BKE_paint_material_layer_stack_from_material(*bmain, material, entries)) {
      return false;
    }
    if (ordinal < 0) {
      return false;
    }
    const PaintMaterialLayerStackEntry *entry = nullptr;
    for (const PaintMaterialLayerStackEntry &candidate : entries) {
      if (candidate.ordinal == ordinal) {
        entry = &candidate;
        break;
      }
    }
    if (entry == nullptr || !entry->is_group || entry->owner_tree == nullptr) {
      return false; /* Only groups can have color tags. */
    }
    /* Find the group's own node in its owner tree: the entry names it by identifier, which is
     * what #PaintMaterialLayerStackEntry::node_id is. The parent's identifier is what the group
     * hangs off inside the enclosing tree, and tagging that would color the wrong folder. */
    bNode *group_node = nullptr;
    for (bNode &node : entry->owner_tree->nodes) {
      if (node.identifier == entry->node_id) {
        group_node = &node;
        break;
      }
    }
    if (group_node == nullptr) {
      return false;
    }
    BKE_paint_material_layer_color_tag_set(*group_node, color_tag);
    /* The node tree changed (IDProperty added/modified), so notify. */
    ID &tree_id = const_cast<ID &>(entry->owner_tree->id);
    DEG_id_tag_update(&tree_id, ID_RECALC_NTREE_OUTPUT);
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &material.id);
    return true;
  }

  bool row_remove(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal) const override
  {
    if (const std::optional<PaintStackRowRoute> correction = this->correction_route_get(
            C, owner, ordinal))
    {
      /* A correction owns no mask image the paint target could be on -- the MASK section it hangs
       * under belongs to its parent -- so the layer path's mask-editing bail-out does not apply.
       * The operator re-activates a neighboring row of its own accord. */
      return this->paint_edit(
          C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
            return BKE_paint_material_layer_correction_remove(
                bmain, material, correction->correction, &error);
          });
    }
    /* Removing the row whose mask is being painted -- or the group holding it -- leaves mask
     * editing first, for the same reason #row_mask_set does. */
    const Scene *scene = CTX_data_scene(&C);
    const Image *edited = (scene != nullptr && scene->toolsettings != nullptr) ?
        scene->toolsettings->paint_mode.mask_image_binding.image :
        nullptr;
    if (edited != nullptr) {
      bool drops_mask = false;
      SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
      if (space_outliner != nullptr && space_outliner->runtime != nullptr) {
        drops_mask = paint_remove_drops_edited_mask(*space_outliner, ordinal, edited);
      }
      else {
        /* No Outliner in context (a bare Python call): fall back to the row's own mask. A group
         * removed this way keeps a stale check cheap rather than exact. */
        Main *bmain = CTX_data_main(&C);
        drops_mask = (bmain != nullptr &&
                      paint_row_mask_image_get(*bmain, paint_owner(owner), ordinal) == edited);
      }
      if (drops_mask) {
        ED_paint_material_mask_edit_end_if_active(C);
      }
    }
    return this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_remove(bmain, material, ordinal, &error);
        });
  }

  bool sub_row_activate(bContext &C,
                        const StackFocus & /*focus*/,
                        ID & /*owner*/,
                        const StackRow & /*row*/,
                        const StackSubRow &sub_row) const override
  {
    if (sub_row.id == nullptr) {
      return false;
    }
    /* An Image Editor the user already has open is where they expect the map to appear. Only when
     * there is none does this take over the Outliner's own area, which is a destructive enough
     * change that guessing it twice would be wrong. */
    ScrArea *area = outliner_image_area_find(C);
    if (area == nullptr) {
      area = CTX_wm_area(&C);
      if (area == nullptr) {
        return false;
      }
      /* Taking the user's Outliner over is a destructive enough guess that it says so. */
      BKE_report(CTX_wm_reports(&C),
                 RPT_INFO,
                 "No Image Editor was open, so this area was turned into one");
      ED_area_newspace(&C, area, SPACE_IMAGE, false);
    }
    SpaceImage *space_image = static_cast<SpaceImage *>(area->spacedata.first);
    ED_space_image_set(CTX_data_main(&C), space_image, id_cast<Image *>(sub_row.id), false);
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_IMAGE, space_image);
    return true;
  }

  bool preview_activate(bContext &C,
                        ID & /*owner*/,
                        const StackRow &row,
                        const StringRef section_id) const override
  {
    Main *bmain = CTX_data_main(&C);
    Scene *scene = CTX_data_scene(&C);
    /* The Paint of the current object mode: a Sculpt Mode material stroke reads its brush from
     * #ToolSettings::sculpt, not #ToolSettings::imapaint. */
    Paint *paint = BKE_paint_get_active_from_context(&C);
    if (bmain == nullptr || scene == nullptr || paint == nullptr) {
      return false;
    }
    const bool changed = paint_material_mask_preview_activate(
        *bmain, *scene, *paint, row, section_id);
    if (changed) {
      WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
      WM_event_add_notifier(&C, NC_BRUSH | NA_EDITED, nullptr);
    }
    return changed;
  }

  bool target_clear(bContext &C) const override
  {
    Scene *scene = CTX_data_scene(&C);
    if (scene == nullptr || scene->toolsettings == nullptr) {
      return false;
    }
    /* Clearing the paint target also leaves mask editing: the mask brush would otherwise keep
     * writing into the now-hidden mask. */
    ED_paint_material_mask_edit_end_if_active(C);
    for (MaterialPaintChannelImageBinding &binding :
         scene->toolsettings->paint_mode.channel_image_bindings)
    {
      BKE_paint_material_channel_binding_set(binding, nullptr);
    }
    WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
    return true;
  }

  std::optional<eObjectMode> focus_object_mode() const override
  {
    return OB_MODE_TEXTURE_PAINT;
  }

  void message_subscribe(const wmRegionMessageSubscribeParams &params) const override
  {
    wmMsgSubscribeValue msg_sub_value_region_tag_redraw{};
    msg_sub_value_region_tag_redraw.owner = params.region;
    msg_sub_value_region_tag_redraw.user_data = params.region;
    msg_sub_value_region_tag_redraw.notify = ED_region_do_msg_notify_tag_redraw;
    WM_msg_subscribe_rna_anon_prop(params.message_bus,
                                      PaintModeSettings,
                                      channel_image_bindings,
                                      &msg_sub_value_region_tag_redraw);
    WM_msg_subscribe_rna_anon_prop(
        params.message_bus, PaintModeSettings, canvas_source, &msg_sub_value_region_tag_redraw);
  }

  bool notifier_invalidates(const wmNotifier &notifier) const override
  {
    /* The rows are read from the node graph, so an edit there may have changed them; the state
     * hash decides whether anything actually did, this only has to prompt the question. */
    if (notifier.category == NC_NODE) {
      /* Every committed graph edit announces itself here as #NA_EDITED -- a link rewire included,
       * via #BKE_main_ensure_invariants. The state hash decides whether any of it actually
       * changed a row. */
      return notifier.action == NA_EDITED;
    }
    if (notifier.category == NC_MATERIAL) {
      return true;
    }
    /* A stroke edits pixels many times a second and changes no row. The one pixel-state fact a
     * row does read is whether a generated map is still blank: the stroke that first touches it
     * ends with #NA_EDITED, and the state hash -- which folds the same blank answer in -- decides
     * that the rows are stale then. */
    if (notifier.category == NC_IMAGE) {
      return ELEM(notifier.action, NA_ADDED, NA_REMOVED, NA_EDITED);
    }
    return false;
  }

  bool focus_will_open(bContext &C, ID &owner) const override
  {
    if (!this->is_editable(owner)) {
      return false;
    }
    Material &material = paint_owner(owner);
    /* Opening another material's stack leaves mask editing when the mask being painted does not
     * belong to it: the highlight moves materials while the stroke would keep writing into the
     * old one's mask. */
    const Scene *scene = CTX_data_scene(&C);
    const Image *edited = (scene != nullptr && scene->toolsettings != nullptr) ?
        scene->toolsettings->paint_mode.mask_image_binding.image :
        nullptr;
    if (edited != nullptr) {
      Main *bmain = CTX_data_main(&C);
      if (bmain == nullptr ||
          !BKE_paint_material_layer_stack_contains_mask(*bmain, material, *edited))
      {
        ED_paint_material_mask_edit_end_if_active(C);
      }
    }
    const bool normalized = BKE_paint_material_layer_bottom_normalize(*CTX_data_main(&C),
                                                                          material);
    /* The conversion no longer hands out layer identities as a side effect; the rows this source
     * is about to read need them whether or not the shape needed normalizing. */
    const bool markers = BKE_paint_material_layer_markers_ensure(material);
    if (normalized) {
      WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    }
    return normalized || markers;
  }

  const StackDropHandler *drop_handler() const override
  {
    return this;
  }

  const StackEditor *editor() const override
  {
    return this;
  }

  const StackGroupingEditor *grouping() const override
  {
    return this;
  }

  const StackColorEditor *color() const override
  {
    return this;
  }

  /**
   * The row \a target is aimed next to, in this material's current graph: -1 when the drop named
   * no row, which every insert here reads as the top of the stack, or
   * #PAINT_STACK_INSERT_REFUSED when the row it named is gone.
   */
  int insert_anchor_get(Main &bmain, const ID &owner, const StackDropTarget &target) const
  {
    if (!target.anchor.is_valid()) {
      return -1;
    }
    const int anchor = paint_layer_ordinal_for_marker(
        bmain, paint_owner(owner), target.anchor.row_id);
    return (anchor < 0) ? PAINT_STACK_INSERT_REFUSED : anchor;
  }

  /**
   * Where \a target puts a new row, in the numbering #PaintMaterialLayerAddParams::ordinal uses:
   * the position the row itself ends up at, rather than the row it sits next to. -1 is the top of
   * the stack, and #PAINT_STACK_INSERT_REFUSED passes through.
   *
   * The two numberings meet here rather than at the call sites: the off-by-one between "next to
   * row N" and "at position N" is invisible until a layer lands one row out. Below the bottom row
   * is position 0, which the stack can hold once its bottom is an ordinary layer rather than a
   * bare Image Texture -- the core refuses the bare case, and says so.
   */
  int insert_position_get(Main &bmain, const ID &owner, const StackDropTarget &target) const
  {
    const int anchor = this->insert_anchor_get(bmain, owner, target);
    if (anchor < 0) {
      return anchor;
    }
    return (target.place == StackMovePlace::Below) ? anchor : anchor + 1;
  }

  /**
   * Three cases: reordering a row of this same stack; an image dropped onto it, which the row
   * under the drop takes as a channel's map, or, over empty space, a new layer takes as its own;
   * and a material dropped on it, which is baked into fresh maps for a Material-kind layer
   * inserted where the drop was aimed -- beside the row under it, or on top when the drop named
   * no row.
   */
  bool can_accept(const StackReadContext &ctx,
                  const ID &owner,
                  const StackDropPayload &payload,
                  const StackDropTarget &target,
                  const char **r_disabled_hint) const override
  {
    if (payload.id_type == ID_IM && payload.id_uid != 0) {
      if (!this->is_editable(owner)) {
        *r_disabled_hint = TIP_("The material of this stack is linked or overridden");
        return false;
      }
      if (!target.anchor.is_valid()) {
        /* Empty space, or the stack's own breadcrumb: a new layer on top takes the image. */
        return true;
      }
      if (target.anchor.owner_uid != owner.session_uid ||
          target.anchor.source_type != SO_STACK_SRC_PAINT_MATERIAL || ctx.bmain == nullptr)
      {
        return false;
      }
      const Material &material = paint_owner(owner);
      const int ordinal = paint_layer_ordinal_for_marker(
          *ctx.bmain, material, target.anchor.row_id);
      if (ordinal < 0) {
        return false;
      }
      if (target.place != StackMovePlace::Into) {
        /* Beside the row, not onto it: a new layer goes there, and what the row beside it is made
         * of does not matter -- a group is as good an anchor as a layer. */
        return true;
      }
      /* A group composites its own sub-stack and has no map of its own for an image to replace. */
      Vector<PaintMaterialLayerStackEntry> entries;
      if (BKE_paint_material_layer_stack_from_material(*ctx.bmain, material, entries)) {
        for (const PaintMaterialLayerStackEntry &entry : entries) {
          if (entry.ordinal == ordinal) {
            if (entry.is_group) {
              *r_disabled_hint = TIP_(
                  "A group composites its own layers; drop the image between rows to add a layer");
              return false;
            }
            return true;
          }
        }
      }
      return false;
    }
    if (payload.id_type == ID_MA && payload.id_uid != 0) {
      /* The Material layer built from the dropped material is created in this stack, so the stack
       * needs the same editability every other edit needs. The dropped material itself is only
       * read for the bake, not changed, so a linked one is fine. */
      if (!this->is_editable(owner)) {
        *r_disabled_hint = TIP_("The material of this stack is linked or overridden");
        return false;
      }
      /* The row the drop is aimed at has to still be there: the new layer is inserted next to it,
       * and silently landing somewhere else is worse than refusing. */
      if (ctx.bmain == nullptr) {
        return true;
      }
      return this->insert_anchor_get(*ctx.bmain, owner, target) != PAINT_STACK_INSERT_REFUSED;
    }
    return payload.source_item.is_valid() &&
           payload.source_item.source_type == SO_STACK_SRC_PAINT_MATERIAL &&
           payload.source_item.owner_uid == owner.session_uid &&
           target.anchor.owner_uid == owner.session_uid && this->can_reorder(owner);
  }

  bool execute(bContext &C,
               const StackFocus &focus,
               ID &owner,
               const StackDropPayload &payload,
               const StackDropTarget &target,
               const wmEvent *event,
               int *r_affected_ordinal) const override
  {
    if (payload.id_type == ID_IM && payload.id_uid != 0) {
      /* The channel the image lands in is the user's choice, made in the popup the assign
       * operator opens. Its extra properties carry the image and where it goes there.
       *
       * Two readings, and the drop's aim is what tells them apart: onto a row, the image becomes
       * that layer's map for the chosen channel; beside a row, it becomes a new layer of its own
       * at that place, which is what the insertion line promised. */
      PointerRNA props = WM_operator_properties_create(
          "OUTLINER_OT_stack_layer_channel_image_assign");
      RNA_int_set(&props, "image_uid", int(payload.id_uid));
      if (target.anchor.is_valid() && target.place == StackMovePlace::Into) {
        char marker_str[UUID_STRING_SIZE];
        BLI_uuid_format(marker_str, target.anchor.row_id);
        RNA_string_set(&props, "layer", marker_str);
      }
      else {
        Main *bmain = CTX_data_main(&C);
        const int insert_position = this->insert_position_get(*bmain, owner, target);
        if (insert_position == PAINT_STACK_INSERT_REFUSED) {
          return false;
        }
        RNA_int_set(&props, "insert_ordinal", insert_position);
      }
      const wmOperatorStatus status = WM_operator_name_call(
          &C,
          "OUTLINER_OT_stack_layer_channel_image_assign",
          wm::OpCallContext::InvokeDefault,
          &props,
                                                            event);
      WM_operator_properties_free(&props);
      return (status & (OPERATOR_FINISHED | OPERATOR_INTERFACE)) != 0;
    }

    if (payload.id_type == ID_MA && payload.id_uid != 0) {
      /* The same gesture as the Add's Material kind: the material is baked into fresh maps, one
       * per channel it feeds, and the Material-kind layer that takes them over is inserted where
       * the drop was aimed -- above the anchor row, or below it. A drop that named no row -- empty
       * space, the breadcrumb -- puts it on top, which is what -1 means here. Undo is the drop
       * operator's own step. */
      Main *bmain = CTX_data_main(&C);
      Material *source_material = id_cast<Material *>(
          BKE_libblock_find_session_uid(bmain, ID_MA, payload.id_uid));
      if (source_material == nullptr) {
        return false;
      }
      const int insert_anchor = this->insert_anchor_get(*bmain, owner, target);
      if (insert_anchor == PAINT_STACK_INSERT_REFUSED) {
        return false;
      }
      /* Below the anchor is a place of its own here, not "above the row under it": under the
       * bottom row there is no such row, and that is exactly where a base coat belongs. */
      const PaintMaterialLayerMovePlace place = (target.place == StackMovePlace::Below) ?
                                                    PaintMaterialLayerMovePlace::Below :
                                                    PaintMaterialLayerMovePlace::Above;
      const int new_ordinal = ed::sculpt_paint::material_layer::add_from_material(
          C, paint_owner(owner), insert_anchor, *source_material, place);
      if (new_ordinal < 0) {
        /* The refusals along the bake path report their own reasons; a refused drop has no
         * operator reports to carry anything further. */
        return false;
      }
      WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &owner);
      /* #OUTLINER_OT_stack_layer_id_drop deliberately carries no #OPTYPE_UNDO -- the same
       * operator also just hands an image drop over to a popup that has not mutated anything
       * yet, and an automatic push at that point would land before the popup's own real step.
       * A material drop's layer lands in the bake's #before_render step, the way the Add's own
       * gesture does it, so this push wraps the whole gesture the same way #OPTYPE_UNDO would. */
      ED_undo_push(&C, "Add Material Layer");
      if (r_affected_ordinal != nullptr) {
        *r_affected_ordinal = new_ordinal;
      }
      return true;
    }

    const char *unused_hint = nullptr;
    /* A real read context, not the empty default: can_accept judges what the drop would land on,
     * and the first source that reads the context from it would otherwise walk into nulls. */
    if (!this->can_accept(
            outliner_stack_read_context(C), owner, payload, target, &unused_hint))
    {
      return false;
    }
    Main *bmain = CTX_data_main(&C);
    const Material &material = paint_owner(owner);
    const int from_ordinal = paint_layer_ordinal_for_marker(
        *bmain, material, payload.source_item.row_id);
    const int anchor_ordinal = paint_layer_ordinal_for_marker(
        *bmain, material, target.anchor.row_id);
    if (from_ordinal < 0 || anchor_ordinal < 0) {
      return false;
    }
    return this->row_move(C, focus, owner, from_ordinal, anchor_ordinal, target.place, nullptr);
  }

  void drop_id_types(Vector<short> &r_types) const override
  {
    r_types.append(ID_IM);
    r_types.append(ID_MA);
  }

  /** Only an image can come from a bare file: a material always lives in a .blend library. */
  bool drop_external_poll(const wmDrag &drag) const override
  {
    if (drag.type != WM_DRAG_PATH) {
      return false;
    }
    const char *path = WM_drag_get_single_path(&drag);
    return path != nullptr && BLI_path_extension_check_array(path, imb_ext_image) &&
           BLI_exists(path);
  }

  /** An image dropped onto a row is assigned to it; a material is always a row of its own,
   * the same as reordering, and has no such reading. */
  bool drop_supports_into(short id_type) const override
  {
    return id_type == ID_IM;
  }

  /**
   * Resolve \a drag into a local image or material. An image may come from a plain local
   * data-block, an asset -- including a bare file-backed one with no full library path, which
   * #ed::asset::resolve_image_from_asset also marks as an asset the first time it is seen -- or a
   * dropped file. A material only ever comes from a .blend library, and always through
   * "Append & Reuse": every drop is meant to hand the source over for editing (see #Add Material
   * Layer's "made local" report below), and two copies of it lying around for no reason would be
   * worse than always sharing one, whatever the drag's own configured import method happens to be.
   */
  ID *drop_resolve(bContext &C, wmDrag &drag) const override
  {
    Main &bmain = *CTX_data_main(&C);

    if (drag.type == WM_DRAG_PATH) {
      if (!this->drop_external_poll(drag)) {
        return nullptr;
      }
      const char *path = WM_drag_get_single_path(&drag);
      Image *image = BKE_image_load_exists(&bmain, path, nullptr);
      if (image != nullptr) {
        id_us_min(&image->id);
      }
      return (image != nullptr) ? &image->id : nullptr;
    }

    if (drag.type == WM_DRAG_ASSET_LIST) {
      const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
      if (asset_drags == nullptr) {
        return nullptr;
      }
      for (const wmDragAssetListItem &item : *asset_drags) {
        const ID_Type item_idtype = item.is_external ?
                                        item.asset_data.external_info->asset->get_id_type() :
                                        (item.asset_data.local_id ?
                                             GS(item.asset_data.local_id->name) :
                                             ID_Type(0));
        if (item_idtype == ID_IM) {
          if (!item.is_external) {
            return item.asset_data.local_id;
          }
          Image *image = ed::asset::resolve_image_from_asset(
              bmain, *item.asset_data.external_info->asset);
          if (image != nullptr && image->id.asset_data == nullptr) {
            ed::asset::image_mark_as_asset(image);
          }
          return (image != nullptr) ? &image->id : nullptr;
        }
        if (item_idtype == ID_MA) {
          if (!item.is_external) {
            return item.asset_data.local_id;
          }
          return ed::asset::asset_local_id_ensure_imported(bmain,
                                                            *item.asset_data.external_info->asset,
                                                            0,
                                                            ASSET_IMPORT_APPEND_REUSE,
                                                            std::nullopt,
                                                            CTX_wm_reports(&C));
        }
      }
      return nullptr;
    }

    /* A plain local data-block or a single asset drag. */
    if (ID *local = WM_drag_get_local_ID_or_import_from_asset(&C, &drag, ID_IM)) {
      return local;
    }
    if (drag.type == WM_DRAG_ASSET) {
      if (wmDragAsset *asset_drag = WM_drag_get_asset_data(&drag, ID_IM)) {
        Image *image = ed::asset::resolve_image_from_asset(bmain, *asset_drag->asset);
        if (image != nullptr && image->id.asset_data == nullptr) {
          ed::asset::image_mark_as_asset(image);
        }
        return (image != nullptr) ? &image->id : nullptr;
      }
    }
    if (ID *local = WM_drag_get_local_ID(&drag, ID_MA)) {
      return local;
    }
    if (drag.type == WM_DRAG_ASSET) {
      if (wmDragAsset *asset_drag = WM_drag_get_asset_data(&drag, ID_MA)) {
        return ed::asset::asset_local_id_ensure_imported(bmain,
                                                         *asset_drag->asset,
                                                         0,
                                                         ASSET_IMPORT_APPEND_REUSE,
                                                         std::nullopt,
                                                         CTX_wm_reports(&C));
      }
    }
    return nullptr;
  }

  std::string drop_tooltip(short id_type,
                           StringRef item_name,
                           StringRef row_name,
                           const StackDropTarget &target) const override
  {
    if (id_type == ID_MA) {
      if (row_name.is_empty()) {
        return fmt::format(fmt::runtime(TIP_("Add {} as a layer group on top")), item_name);
      }
      return (target.place == StackMovePlace::Below) ?
                 fmt::format(
                     fmt::runtime(TIP_("Add {} as a layer group below {}")), item_name, row_name) :
                 fmt::format(
                     fmt::runtime(TIP_("Add {} as a layer group above {}")), item_name, row_name);
    }
    /* An image, or a drag that only resolves at drop time -- a bare file can only ever become an
     * image here, so the wording reads the same either way. */
    if (row_name.is_empty()) {
      return fmt::format(fmt::runtime(TIP_("Add {} as a new layer on top")), item_name);
    }
    switch (target.place) {
      case StackMovePlace::Into:
        return fmt::format(fmt::runtime(TIP_("Assign {} to {}")), item_name, row_name);
      case StackMovePlace::Below:
        return fmt::format(
            fmt::runtime(TIP_("Add {} as a new layer below {}")), item_name, row_name);
      case StackMovePlace::Above:
        break;
    }
    return fmt::format(fmt::runtime(TIP_("Add {} as a new layer above {}")), item_name, row_name);
  }
};

}  // namespace

Map<int, PaintStackRowRoute> paint_stack_routes_build(
    Span<PaintMaterialLayerStackEntry> entries, int &r_first_unaddressable_index)
{
  Map<int, PaintStackRowRoute> routes;
  r_first_unaddressable_index = -1;
  /* Layers and groups own the ordinals the model gave them; a correction's budget descends from
   * the top of the addressable range and stops at the first one of those it runs into. */
  Set<int> occupied;
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    occupied.add(entry.ordinal);
    PaintStackRowRoute route;
    route.layer_ordinal = entry.ordinal;
    routes.add(entry.ordinal, route);
  }

  int next = STACK_ROW_ORDINAL_MAX;
  int correction_index = 0;
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    for (const Vector<PaintMaterialLayerCorrectionEntry> *corrections :
         {&entry.content_corrections, &entry.mask_corrections})
    {
      for (const PaintMaterialLayerCorrectionEntry &correction : *corrections) {
        if (next < 0 || occupied.contains(next)) {
          /* The descent ran into a row a layer owns, or below the addressable range altogether:
           * this correction, and every one after it in the walk, has no ordinal the tree can
           * address. The walk stops here so the same model always budgets the same way. */
          r_first_unaddressable_index = correction_index;
          return routes;
        }
        PaintStackRowRoute route;
        route.is_correction = true;
        route.layer_ordinal = entry.ordinal;
        route.correction = correction.marker;
        route.section = correction.section;
        routes.add(next, route);
        occupied.add(next);
        next--;
        correction_index++;
      }
    }
  }
  return routes;
}

bool paint_row_owns_mask_image(const StackRow &row, const Image *mask_image)
{
  if (mask_image == nullptr) {
    return false;
  }
  for (const StackContentSection &section : row.content_sections) {
    if (section.identifier != "MASK") {
      continue;
    }
    for (const StackSubRow &sub_row : section.sub_rows) {
      if (sub_row.id == &mask_image->id) {
        return true;
      }
    }
  }
  /* A mask correction has no content section of its own; its map is named by the row's preview
   * slot instead (see #paint_correction_row_build): the slot of a mask correction carries the
   * map's session UID under the MASK section id, where a layer's own mask slot names no image at
   * all and never matches. */
  for (const StackRowPreview &slot : row.preview_slots) {
    if (slot.section_id == "MASK" && slot.id_uid == mask_image->id.session_uid) {
      return true;
    }
  }
  return false;
}

bool paint_material_mask_preview_activate(Main &bmain,
                                           Scene &scene,
                                           Paint &paint,
                                           const StackRow &row,
                                           const StringRef section_id)
{
  if (scene.toolsettings == nullptr) {
    return false;
  }
  bool has_paint_preview = false;
  for (const StackRowPreview &slot : row.preview_slots) {
    has_paint_preview |= slot.section_id == section_id;
  }
  if (!has_paint_preview) {
    return false;
  }
  PaintModeSettings &paint_mode = scene.toolsettings->paint_mode;

  if (section_id == "MASK") {
    Image *mask_image = nullptr;
    for (const StackContentSection &section : row.content_sections) {
      if (section.identifier != "MASK") {
        continue;
      }
      for (const StackSubRow &sub_row : section.sub_rows) {
        mask_image = id_cast<Image *>(sub_row.id);
      }
    }
    if (mask_image == nullptr || paint_mode.mask_image_binding.image == mask_image) {
      return false;
    }
    BKE_paint_material_mask_edit_begin_ex(bmain, scene, paint, paint_mode, *mask_image);
    return true;
  }

  if (section_id == "CHANNELS") {
    if (paint_mode.mask_image_binding.image == nullptr) {
      return false;
    }
    BKE_paint_material_mask_edit_end_ex(bmain, scene, paint, paint_mode);
    return true;
  }

  return false;
}

/* -------------------------------------------------------------------- */
/** \name Assign Dropped Image to a Channel
 *
 * The popup the drop handler opens: it lists the channels the material has wired as stacks, and
 * whichever one the user picks is where the image lands -- on the layer it was dropped on, or on
 * a new layer on top when the drop pointed at empty space.
 * \{ */

static const EnumPropertyItem *stack_channel_image_assign_channel_itemf(bContext *C,
                                                                 PointerRNA * /*ptr*/,
                                                                 PropertyRNA * /*prop*/,
                                                                 bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  SpaceOutliner *space_outliner = (C == nullptr) ? nullptr : CTX_wm_space_outliner(C);
  if (space_outliner != nullptr && space_outliner->runtime != nullptr) {
    const StackReadContext ctx = outliner_stack_read_context(*C);
    ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
    if (owner != nullptr) {
      Material &material = paint_owner(*owner);
      Vector<int> channels;
      BKE_paint_material_layer_channels_wired(material, channels);
      for (const int channel : channels) {
        const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
            eMaterialPaintChannel(channel));
        char identifier[8];
        SNPRINTF_UTF8(identifier, "%d", channel);
        EnumPropertyItem item = {};
        item.value = channel;
        item.identifier = BLI_strdup(identifier);
        item.name = BLI_strdup(IFACE_(info.ui_name));
        item.icon = paint_channel_icon(channel);
        RNA_enum_item_add(&items, &items_num, &item);
      }
    }
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
  /* Each menu item calls this operator again with one channel chosen; the extra properties
   * written here carry the image and the target layer along to that call. The menu items run
   * the operator in exec context -- re-invoking would open this popup all over again. */
  layout.operator_context_set(wm::OpCallContext::ExecDefault);
  PointerRNA extra = layout.op_menu_enum(C, op->type, "channel", std::nullopt, ICON_NONE);
  RNA_int_set(&extra, "image_uid", RNA_int_get(op->ptr, "image_uid"));
  RNA_int_set(&extra, "insert_ordinal", RNA_int_get(op->ptr, "insert_ordinal"));
  char layer_marker_str[UUID_STRING_SIZE];
  RNA_string_get(op->ptr, "layer", layer_marker_str);
  RNA_string_set(&extra, "layer", layer_marker_str);
  ui::popup_menu_end(C, pup);
  return OPERATOR_INTERFACE;
}

static wmOperatorStatus stack_channel_image_assign_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);
  ID *owner = outliner_stack_owner_get(outliner_stack_read_context(*C), *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  Material &material = paint_owner(*owner);

  Image *image = id_cast<Image *>(
      BKE_libblock_find_session_uid(bmain, ID_IM, RNA_int_get(op->ptr, "image_uid")));
  if (image == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "The dropped image is gone");
    return OPERATOR_CANCELLED;
  }

  char layer_marker_str[UUID_STRING_SIZE];
  RNA_string_get(op->ptr, "layer", layer_marker_str);

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  int ordinal = -1;
  if (layer_marker_str[0] == '\0') {
    /* No layer named: the image becomes a layer of its own, at the place the drop was aimed --
     * #PaintMaterialLayerAddParams::ordinal is the position the new layer ends up at, and -1 is
     * the top, which is where a drop that pointed at nothing lands. */
    PaintMaterialLayerAddParams params;
    params.ordinal = RNA_int_get(op->ptr, "insert_ordinal");
    const Scene *scene = CTX_data_scene(C);
    if (scene != nullptr && scene->toolsettings != nullptr) {
      params.image_size = scene->toolsettings->paint_mode.new_channel_image_size;
    }
    if (!BKE_paint_material_layer_add(*bmain, material, params, &ordinal, &error)) {
      /* The reason matters here: dropping under a bare base is refused for a reason the user can
       * act on, and "could not add a layer" says nothing about which. */
      BKE_report(op->reports, RPT_ERROR, RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return OPERATOR_CANCELLED;
    }

    /* Strip file extension from the image name when naming the new layer. The layer's name is
     * read from the Image datablock (image->id.name + 2), which includes the file extension when
     * the image was created by dropping a file. The extension is not meaningful for the layer --
     * it describes the source file, not the layer's purpose in the stack. */
    char layer_name_no_ext[MAX_NAME];
    STRNCPY(layer_name_no_ext, image->id.name + 2);
    BLI_path_extension_strip(layer_name_no_ext);
    BKE_paint_material_layer_rename(*bmain, material, ordinal, layer_name_no_ext, &error);
    /* Renaming cannot fail for a layer we just added: the name is valid by construction, and the
     * layer is not the bottom base. If it does fail, the layer keeps the image's name with
     * extension, which is suboptimal but not broken -- no need to roll back the add. */
  }
  else {
    bUUID marker;
    if (!BLI_uuid_parse_string(&marker, layer_marker_str)) {
      return OPERATOR_CANCELLED;
    }
    ordinal = paint_layer_ordinal_for_marker(*bmain, material, marker);
    if (ordinal < 0) {
      BKE_report(op->reports, RPT_ERROR, "The layer the image was dropped on is gone");
      return OPERATOR_CANCELLED;
    }
  }
  if (!BKE_paint_material_layer_channel_image_set(
          *bmain, material, ordinal, RNA_enum_get(op->ptr, "channel"), *image, &error))
  {
    BKE_report(op->reports, RPT_ERROR, "The image could not be assigned to the channel");
    if (layer_marker_str[0] == '\0') {
      /* The layer this exec added above has no map and no user yet: leaving it behind would make
       * a refused assignment read as a silent add. Take it back, so the refusal leaves the stack
       * exactly as it was. */
      BKE_paint_material_layer_remove(*bmain, material, ordinal, nullptr);
    }
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material.id);
  outliner_stack_row_activate(C, *space_outliner, ordinal);
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_channel_image_assign(wmOperatorType *ot)
{
  ot->name = "Assign Image to Channel";
  ot->description = "Assign the dropped image to a channel of the layer, or add a new layer "
                      "for it";
  ot->idname = "OUTLINER_OT_stack_layer_channel_image_assign";

  ot->invoke = stack_channel_image_assign_invoke;
  ot->exec = stack_channel_image_assign_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "channel", rna_enum_dummy_NULL_items, 0, "Channel", "Channel the image paints");
  RNA_def_enum_funcs(prop, stack_channel_image_assign_channel_itemf);
  RNA_def_int(ot->srna, "image_uid", 0, 0, INT32_MAX, "Image", "", 0, INT32_MAX);
  RNA_def_string(ot->srna,
                 "layer",
                 nullptr,
                 UUID_STRING_SIZE,
                 "Layer",
                 "Marker of the layer the image was dropped on; empty adds a layer of its own");
  RNA_def_int(ot->srna,
              "insert_ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Insert Ordinal",
              "Where the layer added for the image goes when no layer is named; -1 is the top of "
              "the stack",
              -1,
              SHRT_MAX);
}

/** \} */

std::unique_ptr<StackSource> stack_source_paint_material_create()
{
  return std::make_unique<PaintMaterialStackSource>();
}

}  // namespace blender::ed::outliner
