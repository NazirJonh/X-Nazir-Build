/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Copying a stack branch, duplicating rows and importing rows from another material. The branch
 * copy is shared with the ordinary duplicate; the import adds the material-boundary work on top:
 * channel sets, object masks and mask replacement.
 */

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_import.hh"

#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_paint_vector.hh"

#include "IMB_imbuf.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_uuid_types.h"

#include "MEM_guardedalloc.h"

#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

/** The new marker copied for \a old, or \a old when the copy stayed outside the branch. */
bUUID paint_layer_marker_remap(const PaintLayerMarkerMap &map, const bUUID &old)
{
  for (const auto &pair : map) {
    if (BLI_uuid_equal(pair.first, old)) {
      return pair.second;
    }
  }
  return old;
}

/** Point the readers of \a row and everything below it at the copied markers in \a map. */
void paint_layer_anchor_point_refs_remap(MaterialPaintLayer &row,
                                         const PaintLayerMarkerMap &map)
{
  if (paint_layer_kind_reads_anchor_point(row)) {
    row.anchor_point_source = paint_layer_marker_remap(map, row.anchor_point_source);
  }
  for (MaterialPaintLayer &effect : row.effects) {
    paint_layer_anchor_point_refs_remap(effect, map);
  }
  for (MaterialPaintLayer &item : row.mask_stack) {
    paint_layer_anchor_point_refs_remap(item, map);
  }
  for (MaterialPaintLayer &child : row.children) {
    paint_layer_anchor_point_refs_remap(child, map);
  }
}

/**
 * Clear the reference of the copied reader \a row when its point is not part of \a ma: a copy
 * from another material would otherwise show a permanent NotFound issue.
 */
void paint_layer_anchor_point_foreign_ref_clear(Material &ma, MaterialPaintLayer &row)
{
  if (paint_layer_kind_reads_anchor_point(row) && !BLI_uuid_is_nil(row.anchor_point_source) &&
      BKE_paint_layers_find(ma, row.anchor_point_source) == nullptr)
  {
    row.anchor_point_source = {};
  }
}

/**
 * A copy of the map \a source with its pixels. #BKE_id_copy copies the image's settings but not the
 * buffer behind it, and a painted map is a buffer that exists nowhere else -- unsaved, generated --
 * so the bare copy would come back blank and the layer made from it would show nothing.
 */
Image *paint_layer_image_duplicate(Main &bmain, Image &source)
{
  Image *copy = id_cast<Image *>(BKE_id_copy(&bmain, &source.id));
  /* A generated map is of type #IMA_TYPE_UV_TEST, not #IMA_TYPE_IMAGE, so the source alone decides
   * whether there is a single buffer to carry over. */
  if (copy == nullptr || !ELEM(source.source, IMA_SRC_FILE, IMA_SRC_GENERATED)) {
    return copy;
  }

  void *source_lock = nullptr;
  ImBuf *source_ibuf = BKE_image_acquire_ibuf(&source, nullptr, &source_lock);
  if (source_ibuf != nullptr) {
    /* The copy's own buffer is what its settings generate or load: same size and kind as the
     * source's, and blank. The pixels go into that buffer rather than replacing it, which keeps
     * everything the image system derived from the settings (tile, colorspace, gen flags). */
    void *copy_lock = nullptr;
    ImBuf *copy_ibuf = BKE_image_acquire_ibuf(copy, nullptr, &copy_lock);
    if (copy_ibuf != nullptr && copy_ibuf->x == source_ibuf->x && copy_ibuf->y == source_ibuf->y) {
      const size_t pixel_num = size_t(source_ibuf->x) * size_t(source_ibuf->y);
      bool copied = false;
      if (source_ibuf->float_data() != nullptr && copy_ibuf->float_data() != nullptr &&
          source_ibuf->channels == copy_ibuf->channels)
      {
        memcpy(copy_ibuf->float_data_for_write(),
               source_ibuf->float_data(),
               sizeof(float) * pixel_num * size_t(copy_ibuf->channels));
        copied = true;
      }
      else if (source_ibuf->byte_data() != nullptr && copy_ibuf->byte_data() != nullptr) {
        memcpy(copy_ibuf->byte_data_for_write(), source_ibuf->byte_data(), pixel_num * 4);
        copied = true;
      }
      if (copied) {
        /* Painted pixels exist only in this buffer, so it has to count as unsaved work. */
        copy_ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
        BKE_image_mark_dirty(copy, copy_ibuf);
      }
    }
    BKE_image_release_ibuf(copy, copy_ibuf, copy_lock);
  }
  BKE_image_release_ibuf(&source, source_ibuf, source_lock);
  return copy;
}

/**
 * Whether \a image is a painted map -- its pixels exist nowhere but this buffer -- rather than a
 * texture it reads. A generated image and one the user painted on since it was loaded are painted;
 * a file-source image that is not dirty is a reference to pixels the file keeps.
 */
static bool paint_layer_image_is_painted(Image &image)
{
  return image.source == IMA_SRC_GENERATED || BKE_image_is_dirty(&image);
}

/** Give \a dst a copy of \a src's channel records, each map following the options' policy. */
static void paint_layer_channels_duplicate(Main &bmain,
                                           MaterialPaintLayer &dst,
                                           const MaterialPaintLayer &src,
                                           const PaintLayerCopyOptions &options)
{
  if (src.channels_num <= 0 || src.channels == nullptr) {
    return;
  }
  dst.channels = MEM_new_array<MaterialPaintLayerChannel>(src.channels_num, __func__);
  dst.channels_num = src.channels_num;
  /* The record is plain data and DNA types delete their copy assignment, so it is copied as one
   * block, the way #BKE_paint_layers_channel_add grows the array. */
  memcpy(dst.channels, src.channels, sizeof(*dst.channels) * src.channels_num);
  for (const int i : IndexRange(src.channels_num)) {
    Image *image = dst.channels[i].image;
    if (image != nullptr) {
      if (paint_layer_image_is_painted(*image)) {
        if (options.image_policy == PaintLayerImageImportPolicy::Strip) {
          /* No map: the record keeps its flat value, which is what the import shows. */
          dst.channels[i].image = nullptr;
          if (options.r_stripped_maps != nullptr) {
            (*options.r_stripped_maps)++;
          }
        }
        else {
          /* Painted in place, so a copy sharing its source's image would turn every stroke on
           * one into a stroke on the other. The fresh image carries the one user that this
           * record is; no separate id_us_plus is taken (as #BKE_id_copy returns it). */
          dst.channels[i].image = paint_layer_image_duplicate(bmain, *image);
        }
      }
      else if (options.image_policy == PaintLayerImageImportPolicy::Copy ||
               image->id.asset_data != nullptr)
      {
        /* Legacy: an external map is copied too, with its pixels. A map that is an asset itself is
         * copied as well, whatever the policy: sharing it would make the importing material depend
         * on another asset, and writing a Smart asset to a library would carry that image along
         * as an asset of its own. The copy is a plain image (no asset metadata) that lives inside
         * the material. */
        dst.channels[i].image = paint_layer_image_duplicate(bmain, *image);
      }
      else {
        /* An external map is read, not painted here: the record shares the source's image and
         * takes a user for it. The copy is plain data, so the source's user is not inherited. */
        id_us_plus(&image->id);
      }
    }
    /* A value ramp is owned sub-data like the map: the copy gets one of its own. */
    dst.channels[i].gradient_ramp = (src.channels[i].gradient_ramp != nullptr) ?
                                        static_cast<ColorBand *>(MEM_dupalloc(
                                            src.channels[i].gradient_ramp)) :
                                        nullptr;
  }
}

/** A copy of \a src's Color ID colours linked on \a dst, each with its own record. */
static void paint_layer_id_colors_duplicate(MaterialPaintLayer &dst, const MaterialPaintLayer &src)
{
  for (const MaterialPaintLayerIdColor &color : src.id_colors) {
    MaterialPaintLayerIdColor *copy = MEM_new<MaterialPaintLayerIdColor>(__func__);
    copy->color[0] = color.color[0];
    copy->color[1] = color.color[1];
    copy->color[2] = color.color[2];
    copy->tolerance = color.tolerance;
    copy->flag = color.flag;
    BLI_addtail(&dst.id_colors, copy);
  }
}

/**
 * A copy of \a src's kind parameters onto \a dst. A stateful kind (a map-coverage operator's
 * tolerance and softness) keeps its values with the copy; the destination's own group, if it has
 * one already, is left alone.
 */
static void paint_layer_properties_duplicate(MaterialPaintLayer &dst, const MaterialPaintLayer &src)
{
  if (src.system_properties != nullptr && dst.system_properties == nullptr) {
    dst.system_properties = IDP_CopyProperty_ex(src.system_properties,
                                                LIB_ID_CREATE_NO_USER_REFCOUNT);
  }
}

/**
 * The copy of a Material row reads a copy of the material \a src reads, so the two rows keep
 * settings of their own. A row that has none, or a material that cannot be copied, is read shared.
 */
static void paint_layer_material_duplicate(Main &bmain,
                                           MaterialPaintLayer &dst,
                                           const MaterialPaintLayer &src)
{
  dst.material = src.material;
  if (dst.material == nullptr) {
    return;
  }
  if (src.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
    if (Material *copy = id_cast<Material *>(BKE_id_copy(&bmain, &src.material->id))) {
      /* The copy was made with one user, which the row takes in its place. */
      dst.material = copy;
      return;
    }
  }
  id_us_plus(&dst.material->id);
}

/**
 * The copy of a Curve Patch effect owns duplicates of the whole payload: the curve container and the
 * brush snapshot are the effect's private content and tool state, so a shared pointer would make two
 * effects edit one set of curves. `BKE_id_copy` hands back one user each, which the copy is.
 */
static void paint_layer_curve_patch_duplicate(Main &bmain,
                                              MaterialPaintLayer &dst,
                                              const MaterialPaintLayer &src)
{
  if (src.curve_patch == nullptr) {
    return;
  }
  MaterialPaintLayerCurvePatch *payload = MEM_new<MaterialPaintLayerCurvePatch>(__func__);
  dst.curve_patch = payload;
  if (src.curve_patch->vectors != nullptr) {
    payload->vectors = id_cast<PaintVector *>(
        BKE_id_copy(&bmain, &src.curve_patch->vectors->id));
  }
  if (src.curve_patch->brush != nullptr) {
    payload->brush = id_cast<Brush *>(BKE_id_copy(&bmain, &src.curve_patch->brush->id));
  }
}

/**
 * An unlinked copy of the mask item or correction \a item: a marker of its own and maps of its own,
 * for the same reason as the channel maps.
 */
MaterialPaintLayer *paint_layer_item_duplicate(Main &bmain,
                                               Material &ma,
                                               const MaterialPaintLayer &item,
                                               PaintLayerMarkerMap *r_map,
                                               const PaintLayerCopyOptions &options)
{
  if ((options.flags & PAINT_LAYER_IMPORT_SKIP_CURVE_PATCH) != 0 &&
      item.source == MA_PAINT_LAYER_SOURCE_CURVE_PATCH)
  {
    if (options.r_dropped_curve_patches != nullptr) {
      (*options.r_dropped_curve_patches)++;
    }
    return nullptr;
  }
  MaterialPaintLayer *item_copy = paint_layer_alloc(ma, item.name);
  item_copy->role = item.role;
  item_copy->source = item.source;
  item_copy->kind_subtype = item.kind_subtype;
  item_copy->blend = item.blend;
  item_copy->flag = item.flag;
  item_copy->opacity = item.opacity;
  /* Still names the original point; the caller remaps it once the copies' markers are known. */
  item_copy->anchor_point_source = item.anchor_point_source;
  /* Display data, copied like the rest of the row's own state. */
  item_copy->color_tag = item.color_tag;
  /* The mesh/UV-tiles choice is general row state; the per-object records are keyed by the old
   * marker and stay with it, so the copy starts with no exclusions. */
  item_copy->object_mask_type = item.object_mask_type;
  /* The channel of its source a mask correction takes its grey from, kept with the copy. */
  item_copy->mask_source_channels = item.mask_source_channels;
  /* The row's UV mapping is part of its state, not of its kind: a pasted fill or fill
   * correction has to keep the offset, scale, rotation, lock and wrap the gizmo edits. */
  item_copy->mapping = dna::shallow_copy(item.mapping);
  /* The per (row, channel) overrides are state too: a pasted correction keeps the blend and
   * opacity it was given in each channel, the way the branch copy below keeps a row's. */
  memcpy(item_copy->channel_settings, item.channel_settings, sizeof(item_copy->channel_settings));
  paint_layer_material_duplicate(bmain, *item_copy, item);
  /* A Curve Patch effect carries its payload (its own curve container and brush snapshot) like a
   * Curve Patch row does: the copy owns duplicates of both, never shared. */
  paint_layer_curve_patch_duplicate(bmain, *item_copy, item);
  paint_layer_channels_duplicate(bmain, *item_copy, item, options);
  paint_layer_id_colors_duplicate(*item_copy, item);
  paint_layer_properties_duplicate(*item_copy, item);
  if (r_map != nullptr) {
    r_map->append({item.marker, item_copy->marker});
  }
  BKE_paint_layers_gradient_copy(*item_copy, item);
  return item_copy;
}

/**
 * Fill the linked \a dst with a copy of \a src's own state, its mask and, for a folder, its
 * children, each row with a marker of its own. \a dst is linked into \a ma before this runs, so
 * every marker handed out here is unique against it and against the rows copied before.
 */
void paint_layer_branch_duplicate(Main &bmain,
                                  Material &ma,
                                  MaterialPaintLayer &dst,
                                  const MaterialPaintLayer &src,
                                  PaintLayerMarkerMap *r_map,
                                  const PaintLayerCopyOptions &options)
{
  dst.flag = src.flag;
  dst.opacity = src.opacity;
  dst.blend = src.blend;
  dst.source = src.source;
  dst.kind_subtype = src.kind_subtype;
  /* A reader of a point inside the branch has to read the copied point, not the original; the
   * caller remaps it once every new marker is known. */
  dst.anchor_point_source = src.anchor_point_source;
  if (r_map != nullptr) {
    r_map->append({src.marker, dst.marker});
  }
  /* A display tag travels with the row: a duplicated group has to read like the one it came from,
   * the way a duplicated collection keeps its color. */
  dst.color_tag = src.color_tag;
  /* The mesh/UV-tiles choice travels too; the per-object records do not -- they are keyed by the
   * old marker, so the copy starts with no exclusions and the flag stays clear. */
  dst.object_mask_type = src.object_mask_type;
  /* Which channel of its source a mask correction reads is part of its state, not of its kind. */
  dst.mask_source_channels = src.mask_source_channels;
  memcpy(dst.channel_settings, src.channel_settings, sizeof(dst.channel_settings));
  /* Keep the row's UV mapping (offset, scale, rotation, lock and wrap) with the copy. */
  dst.mapping = dna::shallow_copy(src.mapping);
  paint_layer_material_duplicate(bmain, dst, src);
  paint_layer_curve_patch_duplicate(bmain, dst, src);
  paint_layer_channels_duplicate(bmain, dst, src, options);
  paint_layer_id_colors_duplicate(dst, src);
  paint_layer_properties_duplicate(dst, src);
  BKE_paint_layers_gradient_copy(dst, src);

  /* The mask and the corrections go with their row, with maps of their own for the same reason as
   * the channel maps. */
  const auto item_duplicate = [&](const MaterialPaintLayer &item,
                                  ListBaseT<MaterialPaintLayer> &r_list) {
    if (MaterialPaintLayer *copy = paint_layer_item_duplicate(
            bmain, ma, item, r_map, options))
    {
      BLI_addtail(&r_list, copy);
    }
  };
  for (const MaterialPaintLayer &item : src.mask_stack) {
    item_duplicate(item, dst.mask_stack);
  }
  for (const MaterialPaintLayer &effect : src.effects) {
    item_duplicate(effect, dst.effects);
  }

  if (BKE_paint_layers_is_folder(src)) {
    for (const MaterialPaintLayer &child : src.children) {
      MaterialPaintLayer *child_copy = paint_layer_alloc(ma, child.name);
      BLI_addtail(&dst.children, child_copy);
      paint_layer_branch_duplicate(bmain, ma, *child_copy, child, r_map, options);
    }
  }
}

MaterialPaintLayer *BKE_paint_layers_duplicate(Main &bmain,
                                               Material &ma,
                                               MaterialPaintLayer *layer)
{
  /* A correction is copied beside itself in its layer's list, with maps of its own. */
  MaterialPaintLayer *effect_owner = (layer != nullptr) ?
                                         BKE_paint_layers_correction_owner(ma, *layer) :
                                         nullptr;
  /* So is a mask correction, in its row's mask; the base mask is copied with its row only. */
  MaterialPaintLayer *item_owner = (layer != nullptr &&
                                    (layer->flag & MA_PAINT_LAYER_MASK_BASE) == 0) ?
                                       BKE_paint_layers_mask_item_owner(ma, *layer) :
                                       nullptr;
  if (effect_owner == nullptr && item_owner == nullptr && !paint_layer_in_stack(ma, layer)) {
    return nullptr;
  }

  MaterialPaintLayer *copy = paint_layer_alloc(ma, layer->name);
  copy->role = layer->role;
  if (effect_owner != nullptr) {
    BLI_insertlinkafter(&effect_owner->effects, layer, copy);
  }
  else if (item_owner != nullptr) {
    BLI_insertlinkafter(&item_owner->mask_stack, layer, copy);
  }
  else {
    PaintLayerList *owner = BKE_paint_layers_owner_list(ma, *layer);
    BLI_insertlinkafter(owner, layer, copy);
  }
  PaintLayerMarkerMap marker_map;
  paint_layer_branch_duplicate(bmain, ma, *copy, *layer, &marker_map);
  paint_layer_anchor_point_refs_remap(*copy, marker_map);

  BKE_paint_layers_tag_edited(ma);
  return copy;
}

/* -------------------------------------------------------------------- */
/** \name Importing rows from another material
 *
 * The copy helpers above are scoped to a material because every ordinary duplicate stays in the
 * material it was made in. A smart asset crosses that boundary: the rows come from the asset's
 * Material, while allocation, markers and anchor points belong to the target. The helpers are
 * reused as they are -- they already read the source and write the destination independently --
 * and the material-specific work (channel set, object masks, mask replacement) is done here.
 * \{ */

/** Every channel the branch of \a row takes part in, as a bitmask over #eMaterialPaintChannel. */
static uint16_t paint_layer_source_channel_mask(const MaterialPaintLayer &row)
{
  uint16_t mask = 0;
  for (const int i : IndexRange(row.channels_num)) {
    const int channel = row.channels[i].channel;
    if (channel >= 0 && channel < PAINT_MATERIAL_CHANNEL_NUM) {
      mask |= uint16_t(1) << channel;
    }
  }
  /* Corrections and children carry content records; mask items deliberately are not walked: their
   * Base Color record is the mask's own grey, kept whether or not Base Color is a content channel. */
  for (const MaterialPaintLayer &effect : row.effects) {
    mask |= paint_layer_source_channel_mask(effect);
  }
  for (const MaterialPaintLayer &child : row.children) {
    mask |= paint_layer_source_channel_mask(child);
  }
  return mask;
}

/** Whether any row of \a row's branch hides anything per object or per UV tile. */
static bool paint_layer_source_has_object_mask(const MaterialPaintLayer &row)
{
  if ((row.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0) {
    return true;
  }
  for (const MaterialPaintLayer &item : row.mask_stack) {
    if (paint_layer_source_has_object_mask(item)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &effect : row.effects) {
    if (paint_layer_source_has_object_mask(effect)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &child : row.children) {
    if (paint_layer_source_has_object_mask(child)) {
      return true;
    }
  }
  return false;
}

/**
 * Drop the channel records of \a row's branch that \a dst's channel set does not take part in.
 * The maps lose this row's user, exactly as removing a record by hand does. Returns the number of
 * dropped records.
 */
static int paint_layer_prune_foreign_channels(Material &dst, MaterialPaintLayer &row)
{
  const uint16_t set = BKE_paint_layers_channel_set_mask_get(dst);
  int removed = 0;
  for (int i = row.channels_num - 1; i >= 0; i--) {
    const int channel = row.channels[i].channel;
    if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
      BLI_assert_msg(false, "A channel record names a valid paint channel");
      continue;
    }
    if ((set & (uint16_t(1) << channel)) == 0 &&
        BKE_paint_layers_channel_remove(dst, &row, eMaterialPaintChannel(channel)))
    {
      removed++;
    }
  }
  /* Corrections and children carry their own records; mask items do not, their Base Color record
   * is the mask's coverage and is not a content channel the set can exclude. */
  for (MaterialPaintLayer &effect : row.effects) {
    removed += paint_layer_prune_foreign_channels(dst, effect);
  }
  for (MaterialPaintLayer &child : row.children) {
    removed += paint_layer_prune_foreign_channels(dst, child);
  }
  return removed;
}

bool BKE_paint_layers_import_rows(Main &bmain,
                                  Material &dst,
                                  Material &src,
                                  const Span<const MaterialPaintLayer *> rows,
                                  const PaintLayerImportParams &params,
                                  PaintLayerImportResult *r_result)
{
  if (rows.is_empty()) {
    return false;
  }

  if (params.mode == PaintLayerImportMode::Mask) {
    if (rows.size() != 1) {
      return false;
    }
    const MaterialPaintLayer &source_row = *rows[0];
    if (source_row.mask_stack.first == nullptr || !paint_layer_in_stack(src, &source_row) ||
        params.anchor == nullptr || !paint_layer_in_stack(dst, params.anchor))
    {
      return false;
    }

    BKE_paint_layers_channels_materialize(dst);

    /* Duplicate first, replace second: a failure while copying must leave the target's mask
     * untouched. The staged items are unlinked, so they carry the destination's markers but
     * neither the target's mask nor its active item sees them until the list is spliced. */
    ListBaseT<MaterialPaintLayer> staged = {nullptr, nullptr};
    PaintLayerMarkerMap marker_map;
    Vector<MaterialPaintLayer *> staged_items;
    int stripped_maps = 0;
    int dropped_curve_patches = 0;
    PaintLayerCopyOptions copy_options;
    copy_options.flags = params.flags;
    copy_options.r_dropped_curve_patches = &dropped_curve_patches;
    copy_options.image_policy = params.image_policy;
    copy_options.r_stripped_maps = &stripped_maps;
    for (const MaterialPaintLayer &item : source_row.mask_stack) {
      MaterialPaintLayer *copy = paint_layer_item_duplicate(
          bmain, dst, item, &marker_map, copy_options);
      if (copy == nullptr) {
        continue;
      }
      BLI_addtail(&staged, copy);
      staged_items.append(copy);
    }
    if (staged_items.is_empty()) {
      return false;
    }

    if ((params.flags & PAINT_LAYER_IMPORT_APPEND_MASK) == 0) {
      BKE_paint_layers_mask_remove(dst, params.anchor);
    }
    /* A target that already has a mask keeps its own base; the imported items go over it. Without
     * one, the first imported item becomes the base, exactly as the first mask item added by hand
     * does. */
    const bool target_has_base = params.anchor->mask_stack.first != nullptr;
    bool first = true;
    for (MaterialPaintLayer *item : staged_items) {
      if (target_has_base) {
        item->flag &= ~MA_PAINT_LAYER_MASK_BASE;
      }
      else if (first) {
        item->flag |= MA_PAINT_LAYER_MASK_BASE;
      }
      else {
        item->flag &= ~MA_PAINT_LAYER_MASK_BASE;
      }
      first = false;
    }
    BLI_movelisttolist(&params.anchor->mask_stack, &staged);
    for (MaterialPaintLayer *item : staged_items) {
      paint_layer_anchor_point_refs_remap(*item, marker_map);
      paint_layer_anchor_point_foreign_ref_clear(dst, *item);
    }

    dst.paint_layers_flag |= MA_PAINT_LAYERED;
    BKE_paint_layers_tag_edited(dst);
    if (r_result != nullptr) {
      *r_result = {};
      r_result->first = params.anchor;
      r_result->rows = int(staged_items.size());
      r_result->dropped_curve_patches = dropped_curve_patches;
      r_result->stripped_maps = stripped_maps;
    }
    return true;
  }

  /* Rows mode. Validate everything before touching the destination: the branch copy itself cannot
   * fail, so a refused row must not leave half an import behind. */
  if (params.anchor != nullptr) {
    if (!paint_layer_anchor_valid(dst, params.anchor, params.place)) {
      return false;
    }
  }
  else if (params.place != PaintLayerPlace::Above) {
    return false;
  }

  uint16_t source_channels = 0;
  bool dropped_object_masks = false;
  for (const MaterialPaintLayer *row : rows) {
    if (row == nullptr || !paint_layer_in_stack(src, row)) {
      return false;
    }
    source_channels |= paint_layer_source_channel_mask(*row);
    dropped_object_masks |= paint_layer_source_has_object_mask(*row);
  }

  /* Freeze the destination's set before the copied records can widen a derived one. A smart asset
   * brings its channels with it; an ordinary copy into a filtered stack keeps the filter and drops
   * the records it excludes. */
  if ((params.flags & PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS) != 0 && source_channels != 0) {
    BKE_paint_layers_channel_set_mask_set(
        dst, BKE_paint_layers_channel_set_mask_get(dst) | source_channels);
  }
  BKE_paint_layers_channels_materialize(dst);

  PaintLayerPlace place = params.place;
  if (params.anchor != nullptr && params.fill_folder && place != PaintLayerPlace::Into &&
      BKE_paint_layers_is_folder(*params.anchor))
  {
    place = PaintLayerPlace::Into;
  }

  PaintLayerMarkerMap marker_map;
  Vector<MaterialPaintLayer *> inserted;
  int stripped_maps = 0;
  int dropped_curve_patches = 0;
  PaintLayerCopyOptions copy_options;
  copy_options.flags = params.flags;
  copy_options.r_dropped_curve_patches = &dropped_curve_patches;
  copy_options.image_policy = params.image_policy;
  copy_options.r_stripped_maps = &stripped_maps;
  MaterialPaintLayer *anchor = params.anchor;
  for (const MaterialPaintLayer *source_row : rows) {
    MaterialPaintLayer *copy = paint_layer_alloc(dst, source_row->name);
    copy->role = source_row->role;
    paint_layer_link(dst, copy, anchor, place);
    paint_layer_branch_duplicate(bmain, dst, *copy, *source_row, &marker_map, copy_options);
    inserted.append(copy);
    /* Above inserts each next row over the previous one; Below and Into keep the original anchor,
     * so the source order is preserved in all three cases. */
    if (anchor != nullptr && place == PaintLayerPlace::Above) {
      anchor = copy;
    }
  }

  int skipped_channels = 0;
  for (MaterialPaintLayer *copy : inserted) {
    paint_layer_anchor_point_refs_remap(*copy, marker_map);
    paint_layer_anchor_point_foreign_ref_clear(dst, *copy);
    if ((params.flags & PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS) == 0) {
      skipped_channels += paint_layer_prune_foreign_channels(dst, *copy);
    }
  }

  dst.paint_layers_flag |= MA_PAINT_LAYERED;
  BKE_paint_layers_tag_edited(dst);
  if (r_result != nullptr) {
    *r_result = {};
    r_result->first = inserted.is_empty() ? nullptr : inserted[0];
    r_result->rows = int(inserted.size());
    r_result->skipped_channels = skipped_channels;
    r_result->dropped_object_masks = dropped_object_masks;
    r_result->dropped_curve_patches = dropped_curve_patches;
    r_result->stripped_maps = stripped_maps;
  }
  return true;
}

/** \} */

}  // namespace blender
