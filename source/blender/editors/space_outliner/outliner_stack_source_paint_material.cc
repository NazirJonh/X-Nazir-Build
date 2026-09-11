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
#include <cstdio>

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
#include "BKE_paint_material_resolve.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLI_assert.h"
#include "BLI_hash.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_listbase_wrapper.hh"
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

#include "ED_image.hh"
#include "ED_material_bake.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"

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

namespace {

/**
 * Session UID of the material whose layer the paint bindings currently point at.
 *
 * Session state, not file state: it exists so a row is highlighted only where it is true, and an
 * undo step that restores different bindings makes it a lie, which is what #undo_reset is for. A
 * UID rather than a pointer, since the material can be freed or remapped between two reads while
 * the UID stays the same name for the same data.
 */
uint32_t g_bindings_owner_uid = 0;

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

void paint_binding_set(MaterialPaintChannelImageBinding &binding, Image *image)
{
  if (binding.image == image) {
    return;
  }
  /* Either end may be null -- an unbound channel, or a binding being cleared -- and taking the
   * address of a member through a null pointer to let #id_us_min sort it out is undefined even
   * where it happens to work. */
  if (binding.image != nullptr) {
    id_us_min(&binding.image->id);
  }
  binding.image = image;
  if (binding.image != nullptr) {
    id_us_plus(&binding.image->id);
  }
  BKE_imageuser_default(&binding.iuser);
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
      if (sub_row.role == role) {
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
StackRowPreview paint_mask_slot_build(const bool keeps_row_icon)
{
  StackRowPreview slot;
  slot.section_id = "MASK";
  slot.icon = ICON_MOD_MASK;
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
    section.sub_rows.append(std::move(sub_row));
  }
  return section;
}

/** The section a mask expands into: the mask image itself. */
StackContentSection paint_mask_section_build(const Image &mask_image)
{
  StackContentSection section;
  section.identifier = "MASK";
  section.name = IFACE_("Mask Content");

  StackSubRow sub_row;
  sub_row.role = PAINT_LAYER_MAP_MASK;
  sub_row.name = mask_image.id.name + 2;
  sub_row.id = const_cast<ID *>(&mask_image.id);
  sub_row.icon = ICON_MOD_MASK;
  section.sub_rows.append(std::move(sub_row));
  return section;
}

class PaintMaterialStackSource final : public StackSource,
                                       public StackEditor,
                                       public StackGroupingEditor,
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
      const PaintMaterialLayerKind row_kind = PaintMaterialLayerKind(entry.kind);
      row.icon = entry.is_group ? (group_material != nullptr ? ICON_MATERIAL : ICON_FILE_FOLDER) :
                 entry.has_mask ? ICON_MOD_MASK :
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
      if (group_material != nullptr) {
        StackRowPreview material_slot;
        material_slot.id_uid = group_material->id.session_uid;
        material_slot.id_type = ID_MA;
        row.preview_slots.append(std::move(material_slot));
      }
      else if (entry.is_group) {
        if (mask_image != nullptr) {
          row.preview_slots.append(paint_mask_slot_build(true));
          row.content_sections.append(paint_mask_section_build(*mask_image));
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
        if (mask_image != nullptr) {
          row.preview_slots.append(paint_mask_slot_build(false));
        }
        row.content_sections.append(paint_channels_section_build(entry));
        if (mask_image != nullptr) {
          row.content_sections.append(paint_mask_section_build(*mask_image));
        }
      }
      if (entry.factor_prop) {
        row.value_ptr = *entry.factor_prop;
        /* #RNA_PaintMaterialLayerOpacity's own "value", always 0-100%; see
         * #layer_model_entry_from_node. */
        row.value_prop = "value";
      }
      if (entry.blend_prop) {
        row.mode_ptr = *entry.blend_prop;
        row.mode_prop = "blend_type";
      }

      r_rows.append(std::move(row));
    }
    /* A group is listed after the rows it holds -- that is the order the reader walks the graph in
     * -- so the enclosing row of a nested one is the first row *after* it that sits one level up.
     * Resolved here, once every row exists. */
    for (const int64_t index : r_rows.index_range()) {
      StackRow &row = r_rows[index];
      if (row.depth == 0) {
        continue;
      }
      for (int64_t next = index + 1; next < r_rows.size(); next++) {
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
                    const int /*ordinal*/,
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

    /* A row that names no map -- a group -- is not something the brush can write into, and
     * clearing the bindings for it would silently take the paint target away from the user. */
    if (!paint_row_has_map(row)) {
      return true;
    }

    PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
    for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
      paint_binding_set(paint_mode.channel_image_bindings[channel],
                        paint_row_map_get(row, channel));
    }
    g_bindings_owner_uid = material.id.session_uid;
    WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
    return true;
  }

  bool row_is_active(const StackReadContext &ctx,
                     const StackFocus & /*focus*/,
                     const ID &owner,
                     const StackRow &row) const override
  {
    if (g_bindings_owner_uid != owner.session_uid || !row.supported) {
      return false;
    }
    /* Without this, every map-less row -- every group -- would answer "yes" the moment no image is
     * bound, because it would compare its own nothing against the bindings' nothing. */
    if (!paint_row_has_map(row)) {
      return false;
    }
    if (ctx.scene == nullptr || ctx.scene->toolsettings == nullptr) {
      return false;
    }
    const PaintModeSettings &paint_mode = ctx.scene->toolsettings->paint_mode;
    for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
      if (paint_mode.channel_image_bindings[channel].image != paint_row_map_get(row, channel)) {
        return false;
      }
    }
    return true;
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
    return this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_reorder(bmain, material, from_ordinal, to_ordinal, &error);
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
  }

  int row_add(bContext &C,
              const StackFocus & /*focus*/,
              ID &owner,
              const int kind,
              const int ordinal) const override
  {
    /* The kinds this editor declares, by their place in #add_kinds' list. */
    const int fill_kind = 1;
    const int material_kind = 2;
    if (kind == material_kind) {
      /* A Material layer is made from a material, and this overload is handed none; the Add
       * resolves one and calls the #StackAddArgs overload instead. */
      return -1;
    }
    PaintMaterialLayerAddParams params;
    params.type = (kind == fill_kind) ? PaintMaterialLayerAddType::Fill :
                                        PaintMaterialLayerAddType::Image;
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

  /**
   * Add a layer whose maps are a picked material baked per channel.
   *
   * \a source is what the Add resolved from its `source` name, so a repeated or scripted call bakes
   * the same material again. Bake first -- the target Images are created on this thread before the
   * bake call returns -- then add the layer with those maps as its own, then mark it Material.
   * Pixels keep arriving in a job after this returns; the row is there from the first redraw.
   */
  int row_add_material(bContext &C, ID &owner, const int ordinal, Material &source) const
  {
    wmWindowManager *wm = CTX_wm_manager(&C);
    Material *picked = &source;
    Main *bmain = CTX_data_main(&C);

    int image_size = 1024;
    const Scene *scene = CTX_data_scene(&C);
    if (scene != nullptr && scene->toolsettings != nullptr) {
      image_size = scene->toolsettings->paint_mode.new_channel_image_size;
    }

    /* The channels the layer can carry: every material channel the picked material
     * actually feeds. Constant channels bake as flat fills without a render; only
     * Unavailable ones are skipped. What the owner's stack has wired does not limit this:
     * missing chains are migrated up front, and an empty owner takes a dedicated path. */
    const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(picked);
    Vector<int> required;
    for (const int channel : PAINT_MATERIAL_LAYER_MATERIAL_CHANNELS) {
      if (resolve.channels[channel] != ChannelResolution::Unavailable) {
        required.append(channel);
      }
    }
    if (required.is_empty()) {
      BKE_reportf(CTX_wm_reports(&C),
                  RPT_WARNING,
                  RPT_("Material \"%s\" feeds none of the paint channels"),
                  picked->id.name + 2);
      return -1;
    }

    Material &target_material = paint_owner(owner);
    const uint64_t revision_before = BKE_material_paint_layer_revision_get(target_material);

    /* TEMP-DEBUG (remove before merge): trace which stage drops the gesture. */
    printf("[MAT_LAYER] required:");
    for (const int channel : required) {
      printf(" %d", channel);
    }
    printf("\n");
    fflush(stdout);

    /* Route E (empty canvas) vs S (existing stack). An ensure refusal for a channel wired
     * to a foreign graph stops the gesture before a single bake image exists. */
    bool use_base = false;
    {
      Vector<PaintMaterialLayerStackEntry> probe;
      if (!BKE_paint_material_layer_stack_from_material(*bmain, target_material, probe)) {
        use_base = true;
      }
      else {
        PaintMaterialLayerEditError ensure_error = PaintMaterialLayerEditError::None;
        if (!BKE_paint_material_layer_channels_ensure(
                *bmain, target_material, required.as_span(), &ensure_error))
        {
          /* TEMP-DEBUG (remove before merge). */
          printf("[MAT_LAYER] ensure failed err=%d\n", int(ensure_error));
          fflush(stdout);
          if (ensure_error == PaintMaterialLayerEditError::NotAStack) {
            use_base = true;
          }
          else {
            BKE_report(CTX_wm_reports(&C),
                       RPT_ERROR,
                       RPT_(BKE_paint_material_layer_edit_error_message(ensure_error)));
            return -1;
          }
        }
      }
      /* TEMP-DEBUG (remove before merge). */
      printf("[MAT_LAYER] route use_base=%d\n", int(use_base));
      fflush(stdout);
    }

    Vector<ed::material_bake::BakeTargetSpec> targets;
    for (const int channel : required) {
      targets.append({eMaterialPaintChannel(channel)});
    }

    ed::material_bake::MaterialBakeToImagesParams bake_params;
    bake_params.material = picked;
    bake_params.targets = targets;
    bake_params.size = image_size;
    bake_params.blocking = false;
    const ed::material_bake::MaterialBakeToImagesResult bake = ed::material_bake::
        material_bake_to_images(*bmain, wm, CTX_wm_window(&C), bake_params);
    /* TEMP-DEBUG (remove before merge). */
    printf("[MAT_LAYER] bake ok=%d created=%d skipped=%d\n",
           int(bake.ok),
           int(bake.created.size()),
           int(bake.skipped_unavailable.size()));
    fflush(stdout);
    if (!bake.ok || bake.created.is_empty()) {
      /* An ensure that already widened the stack stays behind as its own visible change;
       * say so plainly instead of leaving a silent preparation. */
      if (BKE_material_paint_layer_revision_get(paint_owner(owner)) != revision_before) {
        BKE_reportf(CTX_wm_reports(&C),
                    RPT_ERROR,
                    RPT_("Bake of \"%s\" failed after preparing the layer channels; "
                         "no Material layer was added"),
                    picked->id.name + 2);
      }
      return -1;
    }

    /* The baked maps go in as the layer's own maps: no placeholder is created for those channels
     * only to be replaced, and each map's one fresh user becomes the user of the node showing it.
     * The add takes all of them over, so a refused add or a channel the layer does not get leaves
     * nothing behind here to clean up. */
    Vector<PaintMaterialLayerChannelImage> baked_maps;
    for (const int i : bake.created.index_range()) {
      baked_maps.append({int(bake.created_channels[i]), bake.created[i]});
    }

    int new_ordinal = -1;
    this->paint_edit(
        C, owner, [&](Main &edit_bmain, Material &material, PaintMaterialLayerEditError &error) {
          bool step_ok = false;
          if (use_base) {
            /* Path E: the empty canvas becomes one normalized row holding the baked maps. */
            step_ok = BKE_paint_material_layer_add_material_base(
                edit_bmain, material, baked_maps.as_span(), &new_ordinal, &error);
          }
          else {
            PaintMaterialLayerAddParams params;
            params.type = PaintMaterialLayerAddType::Image;
            params.anchor_ordinal = ordinal;
            params.image_size = image_size;
            params.channel_images = baked_maps;
            step_ok = BKE_paint_material_layer_add(
                edit_bmain, material, params, &new_ordinal, &error);
          }
          /* TEMP-DEBUG (remove before merge). */
          printf("[MAT_LAYER] add ok=%d ordinal=%d err=%d\n",
                 int(step_ok),
                 new_ordinal,
                 int(error));
          fflush(stdout);
          if (!step_ok) {
            return false;
          }
          /* The kind marker is what makes the row read as Material rather than as a Paint layer
           * whose maps happen to carry a bake link. */
          if (!BKE_paint_material_layer_kind_set(
                  edit_bmain, material, new_ordinal, PaintMaterialLayerKind::Material, &error))
          {
            /* TEMP-DEBUG (remove before merge). */
            printf("[MAT_LAYER] kind_set failed err=%d\n", int(error));
            fflush(stdout);
            return false;
          }
          /* TEMP-DEBUG (remove before merge). */
          printf("[MAT_LAYER] done ordinal=%d\n", new_ordinal);
          fflush(stdout);
          return true;
        });
    return new_ordinal;
  }

  int row_add(bContext &C,
              const StackFocus &focus,
              ID &owner,
              const int kind,
              const int ordinal,
              const StackAddArgs &args) const override
  {
    /* The kinds this editor declares, by their place in #add_kinds' list. */
    const int fill_kind = 1;
    const int material_kind = 2;
    if (kind == material_kind) {
      /* The Add resolved the source by the ID type this kind declared; anything else is a caller
       * that went around it. */
      if (args.source == nullptr || GS(args.source->name) != ID_MA) {
        return -1;
      }
      return this->row_add_material(C, owner, ordinal, *id_cast<Material *>(args.source));
    }
    if (kind != fill_kind || args.color == nullptr) {
      return this->row_add(C, focus, owner, kind, ordinal);
    }
    PaintMaterialLayerAddParams params;
    params.type = PaintMaterialLayerAddType::Fill;
    copy_v4_v4(params.fill_color, args.color);
    params.anchor_ordinal = ordinal;
    const Scene *scene = CTX_data_scene(&C);
    if (scene != nullptr && scene->toolsettings != nullptr) {
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
    int new_ordinal = -1;
    this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_duplicate(bmain, material, ordinal, &new_ordinal, &error);
        });
    return new_ordinal;
  }

  bool row_rename(bContext &C,
                  const StackFocus & /*focus*/,
                  ID &owner,
                  const int ordinal,
                  const StringRefNull name) const override
  {
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
    int image_size = 1024;
    if (add) {
      const Scene *scene = CTX_data_scene(&C);
      if (scene != nullptr && scene->toolsettings != nullptr) {
        image_size = scene->toolsettings->paint_mode.new_channel_image_size;
      }
    }
    return this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return add ? BKE_paint_material_layer_mask_add(
                           bmain, material, ordinal, initial_color, image_size, &error) :
                       BKE_paint_material_layer_mask_remove(bmain, material, ordinal, &error);
        });
  }

  bool row_fill_color_set(bContext &C,
                           const StackFocus & /*focus*/,
                           ID &owner,
                           const int ordinal,
                           const float color[4],
                           PaintTileMap *session_tiles) const override
  {
    if (session_tiles != nullptr) {
      Material &target_material = paint_owner(owner);
      Main &bmain = *CTX_data_main(&C);
      paint_fill_session_ensure(bmain, target_material, ordinal, *session_tiles);
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
                              PaintTileMap *session_tiles) const override
  {
    if (session_tiles != nullptr) {
      Material &target_material = paint_owner(owner);
      Main &bmain = *CTX_data_main(&C);
      paint_fill_session_ensure(bmain, target_material, ordinal, *session_tiles);
    }
    return this->paint_preview(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          return BKE_paint_material_layer_fill_color_preview(
              bmain, material, ordinal, color, &error);
        });
  }

  int rows_group(bContext &C,
                 const StackFocus & /*focus*/,
                 ID &owner,
                 const int from_ordinal,
                 const int to_ordinal) const override
  {
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
          return BKE_paint_material_layer_group_ungroup(bmain, material, ordinal, &layer_num, &error);
        });
    return layer_num;
  }

  bool row_color_tag_set(bContext &C,
                         const StackFocus & /*focus*/,
                         ID &owner,
                         const int ordinal,
                         const int color_tag) const override
  {
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
    return this->paint_edit(
        C, owner, [&](Main &bmain, Material &material, PaintMaterialLayerEditError &error) {
          if (!BKE_paint_material_layer_remove(bmain, material, ordinal, &error)) {
            return false;
          }
          /* The removed layer may well have been the paint target. */
          g_bindings_owner_uid = 0;
          return true;
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

  bool target_clear(bContext &C) const override
  {
    Scene *scene = CTX_data_scene(&C);
    if (scene == nullptr || scene->toolsettings == nullptr) {
      return false;
    }
    for (MaterialPaintChannelImageBinding &binding :
         scene->toolsettings->paint_mode.channel_image_bindings)
    {
      paint_binding_set(binding, nullptr);
    }
    g_bindings_owner_uid = 0;
    WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
    return true;
  }

  void undo_reset() const override
  {
    /* Session UIDs survive undo, addresses do not, and the bindings this pointer claims to own may
     * have been restored to something else entirely. */
    g_bindings_owner_uid = 0;
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
    WM_msg_subscribe_rna_anon_prop(
        params.message_bus, PaintModeSettings, channel_image_bindings, &msg_sub_value_region_tag_redraw);
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
    const bool normalized = BKE_paint_material_layer_bottom_normalize(*CTX_data_main(&C), material);
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
   * and a material dropped on it, which a group standing for that material takes, inserted where
   * the drop was aimed -- beside the row under it, or on top when the drop named no row.
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
      /* The group standing for the material is created in this stack, so its material needs the
       * same editability every other edit needs. The dropped material itself is only referenced,
       * not changed, so a linked one is fine. */
      if (!this->is_editable(owner)) {
        *r_disabled_hint = TIP_("The material of this stack is linked or overridden");
        return false;
      }
      /* The row the drop is aimed at has to still be there: the group is inserted next to it, and
       * silently landing somewhere else is worse than refusing. */
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
      const wmOperatorStatus status = WM_operator_name_call(&C,
                                                            "OUTLINER_OT_stack_layer_channel_image_assign",
                                                            wm::OpCallContext::InvokeDefault,
                                                            &props,
                                                            event);
      WM_operator_properties_free(&props);
      return (status & (OPERATOR_FINISHED | OPERATOR_INTERFACE)) != 0;
    }

    if (payload.id_type == ID_MA && payload.id_uid != 0) {
      /* The group that stands for the material is inserted where the drop was aimed, the way a
       * reorder lands: above the anchor row, or below it. A drop that named no row -- empty
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
      PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
      int new_ordinal = -1;
      if (!BKE_paint_material_layer_group_material_add(*bmain,
                                                       paint_owner(owner),
                                                       *source_material,
                                                       insert_anchor,
                                                       place,
                                                       &new_ordinal,
                                                       &error))
      {
        /* A refused drop has no operator reports to carry the reason; the reports are where a
         * scripted or shortcut-driven call looks next, and the tooltip said why while dragging. */
        BKE_report(CTX_wm_reports(&C),
                   RPT_ERROR,
                   RPT_(BKE_paint_material_layer_edit_error_message(error)));
        return false;
      }
      WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &owner);
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
};

}  // namespace

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
    params.type = PaintMaterialLayerAddType::Image;
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
  ot->description = "Assign the dropped image to a channel of the layer, or add a new layer for it";
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
