/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Paint Vector ID implementation. The item arrays are owned flat arrays, so the whole
 * document is written / read in one custom pass; the embedded `PaintShapeSettings` snapshot owns
 * its profiles (`CurveMapping`) and color ramps (`ColorBand`), which are handled explicitly here.
 */

#include "BKE_paint_vector.hh"

#include "MEM_guardedalloc.h"

#include "DNA_paint_vector_types.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_utildefines.h"

#include "BKE_colortools.hh"
#include "BKE_idprop.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"

#include "DNA_material_types.h"

#include "BLO_read_write.hh"

#include "BLT_translation.hh"

#include <type_traits>

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Item helpers
 * \{ */

/* Every shallow copy of a #PaintVectorItem or #PaintShapeSettings below relies on the whole
 * struct being bitwise copyable, and on the copy sites then re-duplicating every owned pointer
 * (points / splines, stroke_profile / fill_profile / stroke_ramp). Adding an
 * owned field to either DNA struct requires updating all of those sites; these asserts keep the
 * bitwise assumption explicit so a non-trivial member cannot sneak in silently. */
static_assert(std::is_trivially_copyable_v<PaintVectorItem>,
              "PaintVectorItem is shallow-copied in this file; every owned pointer it gains must "
              "be re-duplicated by the copy sites below");
static_assert(std::is_trivially_copyable_v<PaintShapeSettings>,
              "PaintShapeSettings is shallow-copied in this file; every owned pointer it gains "
              "must be re-duplicated by the copy sites below");

static void paint_vector_item_points_free(PaintVectorItem &item)
{
  MEM_SAFE_DELETE(item.points);
  item.points_num = 0;
}

static void paint_vector_item_splines_free(PaintVectorItem &item)
{
  MEM_SAFE_DELETE(item.splines);
  item.splines_num = 0;
}

void BKE_paint_vector_style_free(PaintShapeSettings &style)
{
  if (style.stroke_profile != nullptr) {
    BKE_curvemapping_free(style.stroke_profile);
    style.stroke_profile = nullptr;
  }
  if (style.fill_profile != nullptr) {
    BKE_curvemapping_free(style.fill_profile);
    style.fill_profile = nullptr;
  }
  MEM_SAFE_DELETE(style.stroke_ramp);
}

void BKE_paint_vector_style_copy(PaintShapeSettings &dst, const PaintShapeSettings &src)
{
  const CurveMapping *stroke_profile = src.stroke_profile;
  const CurveMapping *fill_profile = src.fill_profile;
  const ColorBand *stroke_ramp = src.stroke_ramp;

  /* Shallow copy first, then duplicate every owned pointer so both snapshots stay independent. */
  dst = src;

  dst.stroke_profile = stroke_profile ? BKE_curvemapping_copy(stroke_profile) : nullptr;
  dst.fill_profile = fill_profile ? BKE_curvemapping_copy(fill_profile) : nullptr;
  if (stroke_ramp != nullptr) {
    dst.stroke_ramp = MEM_dupalloc(stroke_ramp);
  }
}

/** Deep copy of an embedded style snapshot, replacing whatever \a item already owned. */
void BKE_paint_vector_item_style_copy(PaintVectorItem &item, const PaintShapeSettings &src)
{
  BKE_paint_vector_style_free(item.style);
  BKE_paint_vector_style_copy(item.style, src);
}

static void paint_vector_item_free(PaintVectorItem &item)
{
  paint_vector_item_points_free(item);
  paint_vector_item_splines_free(item);
  BKE_paint_vector_style_free(item.style);
}

void BKE_paint_vector_items_clear(PaintVector &pv)
{
  for (int i = 0; i < pv.items_num; i++) {
    paint_vector_item_free(pv.items[i]);
  }
  MEM_SAFE_DELETE(pv.items);
  pv.items_num = 0;
  pv.active_item = 0;
}

PaintVectorItem &BKE_paint_vector_item_add(PaintVector &pv)
{
  PaintVectorItem *items = MEM_new_array<PaintVectorItem>(pv.items_num + 1, __func__);
  /* Shallow move of the old array into the bigger one: the items keep their owned arrays and the
   * old array is freed without walking them, so no duplication is needed here. */
  for (int i = 0; i < pv.items_num; i++) {
    items[i] = pv.items[i];
  }
  MEM_delete(pv.items);
  pv.items = items;
  pv.items_num++;
  pv.active_item = pv.items_num - 1;
  return pv.items[pv.items_num - 1];
}

MutableSpan<PaintVectorPoint> BKE_paint_vector_item_points_alloc(PaintVectorItem &item,
                                                                const int num)
{
  paint_vector_item_points_free(item);
  if (num <= 0) {
    return {};
  }
  item.points = MEM_new_array<PaintVectorPoint>(num, __func__);
  item.points_num = num;
  return MutableSpan<PaintVectorPoint>(item.points, num);
}

MutableSpan<PaintVectorSpline> BKE_paint_vector_item_splines_alloc(PaintVectorItem &item,
                                                                  const int num)
{
  paint_vector_item_splines_free(item);
  if (num <= 0) {
    return {};
  }
  item.splines = MEM_new_array<PaintVectorSpline>(num, __func__);
  item.splines_num = num;
  return MutableSpan<PaintVectorSpline>(item.splines, num);
}

void BKE_paint_vector_item_style_init(PaintVectorItem &item)
{
  BKE_paint_shape_settings_init(&item.style);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Material link
 * \{ */

/** The ID as a PaintVector only when it really is one; custom properties can hold any ID. */
static PaintVector *paint_vector_from_id(ID *id)
{
  if (id == nullptr || GS(id->name) != PaintVector::id_type) {
    return nullptr;
  }
  return reinterpret_cast<PaintVector *>(id);
}

static IDProperty *paint_vector_material_group(const Material &ma, const bool create)
{
  if (!create) {
    IDProperty *root = IDP_GetProperties(const_cast<ID *>(&ma.id));
    if (root == nullptr) {
      return nullptr;
    }
    return IDP_GetPropertyTypeFromGroup(root, PAINT_VECTOR_MATERIAL_GROUP, IDP_GROUP);
  }
  IDProperty *root = IDP_EnsureProperties(const_cast<ID *>(&ma.id));
  IDProperty *group = IDP_GetPropertyTypeFromGroup(root, PAINT_VECTOR_MATERIAL_GROUP, IDP_GROUP);
  if (group == nullptr) {
    IDPropertyTemplate val{0};
    group = IDP_New(IDP_GROUP, &val, PAINT_VECTOR_MATERIAL_GROUP);
    IDP_AddToGroup(root, group);
  }
  return group;
}

/** The group property referencing \a pv, or null. The reference is matched by the ID pointer, so
 * renaming the PaintVector never orphans it. */
static IDProperty *paint_vector_material_prop_for_id(IDProperty *group, const PaintVector &pv)
{
  for (IDProperty &prop : group->data.group) {
    if (prop.type == IDP_ID && IDP_ID_get(&prop) == &pv.id) {
      return &prop;
    }
  }
  return nullptr;
}

void BKE_paint_vector_material_link(Material &ma, PaintVector &pv)
{
  IDProperty *group = paint_vector_material_group(ma, true);
  if (paint_vector_material_prop_for_id(group, pv) != nullptr) {
    return;
  }
  /* First unused stable key (`vector_0`, `vector_1`, ...). Deliberately not the ID name: a rename
   * must not break lookup or create a duplicate link. */
  char key[32];
  for (int n = 0;; n++) {
    SNPRINTF(key, "vector_%d", n);
    if (IDP_GetPropertyFromGroup(group, key) == nullptr) {
      break;
    }
  }
  IDPropertyTemplate val{0};
  val.id = &pv.id;
  IDProperty *prop = IDP_New(IDP_ID, &val, key);
  IDP_AddToGroup(group, prop);
}

void BKE_paint_vector_material_unlink(Material &ma, const PaintVector &pv)
{
  IDProperty *group = paint_vector_material_group(ma, false);
  if (group == nullptr) {
    return;
  }
  IDProperty *prop = paint_vector_material_prop_for_id(group, pv);
  if (prop != nullptr) {
    IDP_FreeFromGroup(group, prop);
  }
}

PaintVector *BKE_paint_vector_material_get(const Material &ma, const char *name)
{
  IDProperty *group = paint_vector_material_group(ma, false);
  if (group == nullptr) {
    return nullptr;
  }
  /* Lookup is by the PaintVector's *name*, independent of the storage key. */
  for (IDProperty &prop : group->data.group) {
    if (prop.type != IDP_ID) {
      continue;
    }
    PaintVector *pv = paint_vector_from_id(IDP_ID_get(&prop));
    if (pv != nullptr && STREQ(pv->id.name + 2, name)) {
      return pv;
    }
  }
  return nullptr;
}

int BKE_paint_vector_material_count(const Material &ma)
{
  IDProperty *group = paint_vector_material_group(ma, false);
  if (group == nullptr) {
    return 0;
  }
  int count = 0;
  for (const IDProperty &prop : group->data.group) {
    if (prop.type == IDP_ID && paint_vector_from_id(IDP_ID_get(const_cast<IDProperty *>(&prop)))) {
      count++;
    }
  }
  return count;
}

PaintVector *BKE_paint_vector_material_get_index(const Material &ma, const int index)
{
  IDProperty *group = paint_vector_material_group(ma, false);
  if (group == nullptr || index < 0) {
    return nullptr;
  }
  int i = 0;
  for (IDProperty &prop : group->data.group) {
    PaintVector *pv = (prop.type == IDP_ID) ? paint_vector_from_id(IDP_ID_get(&prop)) : nullptr;
    if (pv == nullptr) {
      continue;
    }
    if (i == index) {
      return pv;
    }
    i++;
  }
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name IDType callbacks
 * \{ */

static void paint_vector_copy_data(Main * /*bmain*/,
                                   std::optional<Library *> /*owner_library*/,
                                   ID *id_dst,
                                   const ID *id_src,
                                   const int /*flag*/)
{
  PaintVector *pv_dst = id_cast<PaintVector *>(id_dst);
  const PaintVector *pv_src = id_cast<const PaintVector *>(id_src);

  /* The framework may have shallow-copied the source pointers into the copy; drop them so an
   * empty source cannot make free_data free the source's array. */
  pv_dst->items = nullptr;
  pv_dst->items_num = 0;

  if (pv_src->items_num <= 0) {
    return;
  }
  pv_dst->items = MEM_new_array<PaintVectorItem>(pv_src->items_num, __func__);
  pv_dst->items_num = pv_src->items_num;
  pv_dst->active_item = pv_src->active_item;
  pv_dst->flag = pv_src->flag;

  for (int i = 0; i < pv_src->items_num; i++) {
    const PaintVectorItem &src = pv_src->items[i];
    PaintVectorItem &dst = pv_dst->items[i];

    const int points_num = src.points_num;
    const int splines_num = src.splines_num;
    const PaintVectorPoint *points = src.points;
    const PaintVectorSpline *splines = src.splines;
    const PaintShapeSettings style = src.style;

    dst = src;
    /* Restore the source pointers the shallow copy above overwrote, then duplicate the arrays.
     * Every owned pointer must be reset here and re-duplicated below; a new owned field has to
     * be added to both of these steps (and to the static_asserts at the top of this file). */
    dst.points = nullptr;
    dst.splines = nullptr;
    dst.points_num = 0;
    dst.splines_num = 0;
    dst.style.stroke_profile = nullptr;
    dst.style.fill_profile = nullptr;
    dst.style.stroke_ramp = nullptr;

    if (points_num > 0) {
      dst.points = MEM_dupalloc(points);
      dst.points_num = points_num;
    }
    if (splines_num > 0) {
      dst.splines = MEM_dupalloc(splines);
      dst.splines_num = splines_num;
    }
    BKE_paint_vector_item_style_copy(dst, style);
  }
}

static void paint_vector_free_data(ID *id)
{
  PaintVector *pv = id_cast<PaintVector *>(id);
  BKE_paint_vector_items_clear(*pv);
}

static void paint_vector_foreach_id(ID *id, LibraryForeachIDData *data)
{
  PaintVector *pv = id_cast<PaintVector *>(id);
  for (int i = 0; i < pv->items_num; i++) {
    PaintShapeSettings &style = pv->items[i].style;
    /* The curve source and texture assignments are snapshot references used only to redraw the
     * item; they are not owned by the vector document, so keep them non-counting references. */
    BKE_LIB_FOREACHID_PROCESS_ID(data, style.curve_source_collection, IDWALK_CB_NOP);
    BKE_LIB_FOREACHID_PROCESS_ID(data, style.curve_source_object, IDWALK_CB_NOP);
    BKE_LIB_FOREACHID_PROCESS_ID(data, style.fill_texture.image, IDWALK_CB_NOP);
    BKE_LIB_FOREACHID_PROCESS_ID(data, style.fill_texture.material, IDWALK_CB_NOP);
    BKE_LIB_FOREACHID_PROCESS_ID(data, style.fill_texture.tex, IDWALK_CB_NOP);
    BKE_LIB_FOREACHID_PROCESS_ID(data, style.stroke_texture.image, IDWALK_CB_NOP);
    BKE_LIB_FOREACHID_PROCESS_ID(data, style.stroke_texture.material, IDWALK_CB_NOP);
    BKE_LIB_FOREACHID_PROCESS_ID(data, style.stroke_texture.tex, IDWALK_CB_NOP);
  }
}

static void paint_vector_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  PaintVector *pv = id_cast<PaintVector *>(id);

  writer->write_id_struct(id_address, pv);
  BKE_id_blend_write(writer, &pv->id);

  /* The items themselves are one array block, matching the single
   * `BLO_read_array_and_validate_size` in the reader. Writing each item as its own block would
   * make the reader see an array of count 1 and drop every item but a lone one. */
  if (pv->items_num > 0) {
    writer->write_struct_array(pv->items_num, pv->items);
  }
  for (int i = 0; i < pv->items_num; i++) {
    PaintVectorItem &item = pv->items[i];
    if (item.points_num > 0) {
      writer->write_struct_array(item.points_num, item.points);
    }
    if (item.splines_num > 0) {
      writer->write_struct_array(item.splines_num, item.splines);
    }
    if (item.style.stroke_profile != nullptr) {
      BKE_curvemapping_blend_write(writer, item.style.stroke_profile);
    }
    if (item.style.fill_profile != nullptr) {
      BKE_curvemapping_blend_write(writer, item.style.fill_profile);
    }
    if (item.style.stroke_ramp != nullptr) {
      writer->write_struct(item.style.stroke_ramp);
    }
  }
}

static void paint_vector_blend_read_data(BlendDataReader *reader, ID *id)
{
  PaintVector *pv = id_cast<PaintVector *>(id);

  BLO_read_array_and_validate_size(reader, &pv->items, &pv->items_num);
  for (int i = 0; i < pv->items_num; i++) {
    PaintVectorItem &item = pv->items[i];

    BLO_read_array_and_validate_size(reader, &item.points, &item.points_num);
    BLO_read_array_and_validate_size(reader, &item.splines, &item.splines_num);

    BLO_read_struct(reader, CurveMapping, &item.style.stroke_profile);
    if (item.style.stroke_profile != nullptr) {
      BKE_curvemapping_blend_read(reader, item.style.stroke_profile);
      BKE_curvemapping_init(item.style.stroke_profile);
    }
    BLO_read_struct(reader, CurveMapping, &item.style.fill_profile);
    if (item.style.fill_profile != nullptr) {
      BKE_curvemapping_blend_read(reader, item.style.fill_profile);
      BKE_curvemapping_init(item.style.fill_profile);
    }
    BLO_read_struct(reader, ColorBand, &item.style.stroke_ramp);
  }
}

IDTypeInfo IDType_ID_PV = {
    .id_code = PaintVector::id_type,
    .id_filter = FILTER_ID_PV,
    /* Curve source and texture snapshot references only; the ID itself owns no images or
     * materials. */
    .dependencies_id_types = FILTER_ID_GR | FILTER_ID_OB | FILTER_ID_IM | FILTER_ID_MA |
                             FILTER_ID_TE,
    .main_listbase_index = INDEX_ID_PV,
    .struct_size = sizeof(PaintVector),
    .name = "PaintVector",
    .name_plural = N_("paint_vectors"),
    .translation_context = BLT_I18NCONTEXT_ID_PAINTVECTOR,
    /* NO_ANIMDATA: without it the anim-data code would read the member after `ID id` (here the
     * `items` pointer) as an `AnimData *`, corrupting the item array on copy / animation /
     * outliner. Same reason Brush / Palette / PaintCurve set it. */
    .flags = IDTYPE_FLAGS_APPEND_IS_REUSABLE | IDTYPE_FLAGS_NO_ANIMDATA,
    .asset_type_info = nullptr,

    .init_data = nullptr,
    .copy_data = paint_vector_copy_data,
    .free_data = paint_vector_free_data,
    .make_local = nullptr,
    .foreach_id = paint_vector_foreach_id,
    .foreach_cache = nullptr,
    .foreach_path = nullptr,
    .foreach_working_space_color = nullptr,
    .owner_pointer_get = nullptr,

    .blend_write = paint_vector_blend_write,
    .blend_read_data = paint_vector_blend_read_data,
    .blend_read_after_liblink = nullptr,

    .blend_read_undo_preserve = nullptr,

    .lib_override_apply_post = nullptr,
};

/** \} */

PaintVector *BKE_paint_vector_add(Main *bmain, const char *name)
{
  return BKE_id_new<PaintVector>(bmain, name);
}

void BKE_paint_vector_tag_changed(PaintVector &pv)
{
  pv.bake_revision++;
}

void BKE_paint_vector_tag_baked(PaintVector &pv)
{
  pv.flag |= PAINT_VECTOR_BAKED;
  pv.baked_revision = pv.bake_revision;
}

bool BKE_paint_vector_was_baked(const PaintVector &pv)
{
  return (pv.flag & PAINT_VECTOR_BAKED) != 0;
}

bool BKE_paint_vector_needs_rebake(const PaintVector &pv)
{
  return pv.bake_revision != pv.baked_revision;
}

}  // namespace blender
