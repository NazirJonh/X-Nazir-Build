/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The raster side of a Text Decal effect: the rasterized styled text of its Text object is the
 * mask of the whole effect, kept in a coverage map the effect owns (#MaterialPaintLayerText::coverage).
 * The effect works like a Fill otherwise: each channel holds a constant value or the texture the
 * user dropped on it, and the generator and the CPU composite scale what the channels lay by the
 * coverage. The raster therefore depends only on the text, its formatting and the bake options; a
 * channel value, colour, texture or blend never reaches it. The CPU renders on the main thread (the
 * BLF font map is global). A mask item has no channel values and keeps its coverage as the grey of
 * its Base Color map. Like a Warp mapping this is parallel to the bake planner: the planner bakes
 * rows, not effects, and the text raster is not a row bake.
 */

#include "BKE_paint_layers.hh"
#include "BKE_vfont_render.hh"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "BLI_hash_mm2a.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_threads.h"
#include "BLI_time.h"
#include "BLI_timer.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_vfont.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DNA_ID.h"
#include "DNA_curve_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_packedFile_types.h"
#include "DNA_vfont_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_layers_bake_intern.hh"
#include "paint_layers_hash_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_text_decal_intern.hh"

namespace blender {

/** Bumped when what a Text Decal raster stores changes meaning, so older caches invalidate once. */
constexpr uint64_t TEXT_DECAL_HASH_VERSION = 4;

/**
 * Everything the text raster depends on: the source object's identity, its content and formatting,
 * and the payload's bake options. The object's transform is deliberately left out -- the raster
 * lives in the text's own space, and the projection frame is what places it.
 */
static uint64_t text_decal_hash(const MaterialPaintLayerText &payload, const Object &object)
{
  uint64_t h = bake_hash_mix(uint64_t(0x9e3779b97f4a7c15ull), TEXT_DECAL_HASH_VERSION);
  h = bake_hash_mix(h, object.id.session_uid);
  h = bake_hash_mix(h, uint32_t(payload.size));
  h = bake_hash_float(h, payload.padding);
  h = bake_hash_mix(h, uint8_t(payload.fit));
  const Curve *cu = id_cast<const Curve *>(object.data);
  if (cu == nullptr) {
    return h;
  }
  h = bake_hash_float(h, cu->fsize);
  h = bake_hash_float(h, cu->spacing);
  h = bake_hash_float(h, cu->wordspace);
  h = bake_hash_float(h, cu->linedist);
  h = bake_hash_float(h, cu->shear);
  h = bake_hash_float(h, cu->xof);
  h = bake_hash_float(h, cu->yof);
  h = bake_hash_float(h, cu->ulpos);
  h = bake_hash_float(h, cu->ulheight);
  h = bake_hash_float(h, cu->linewidth);
  h = bake_hash_float(h, cu->smallcaps_scale);
  h = bake_hash_mix(h, uint8_t(cu->spacemode));
  h = bake_hash_mix(h, uint8_t(cu->align_y));
  h = bake_hash_mix(h, uint32_t(cu->totbox));
  for (const int i : IndexRange(cu->totbox)) {
    const TextBox &box = cu->tb[i];
    h = bake_hash_float(h, box.x);
    h = bake_hash_float(h, box.y);
    h = bake_hash_float(h, box.w);
    h = bake_hash_float(h, box.h);
  }
  if (const EditFont *ef = cu->editfont) {
    /* In Edit Mode the live text is the edit buffer; `Curve::str` only catches up on exit. */
    h = bake_hash_mix(h, uint32_t(ef->len));
    for (const int i : IndexRange(ef->len)) {
      h = bake_hash_mix(h, uint32_t(ef->textbuf[i]));
      const CharInfo &info = ef->textbufinfo[i];
      h = bake_hash_mix(h, uint8_t(info.flag));
      h = bake_hash_float(h, info.kern);
    }
  }
  else {
    for (const int i : IndexRange(cu->len)) {
      h = bake_hash_mix(h, uint8_t(cu->str[i]));
    }
    for (const int i : IndexRange(cu->len_char32)) {
      const CharInfo &info = cu->strinfo[i];
      h = bake_hash_mix(h, uint8_t(info.flag));
      h = bake_hash_float(h, info.kern);
    }
  }
  for (const VFont *font : {cu->vfont, cu->vfontb, cu->vfonti, cu->vfontbi}) {
    h = bake_hash_mix(h, font != nullptr ? uint64_t(font->id.session_uid) : uint64_t(0));
    if (font == nullptr) {
      continue;
    }
    h = bake_hash_mix(h,
                      BLI_hash_mm2(reinterpret_cast<const uchar *>(font->filepath),
                                   strlen(font->filepath),
                                   0));
    if (font->packedfile != nullptr) {
      /* Hash the payload's identity rather than its bytes: a packed font is megabytes, and this
       * runs for every Text Decal row on every scene update, animation frames included. The
       * session UID changes when the font is re-read and the pointer when the packed data is
       * replaced, which is when the raster has to be rebuilt. */
      h = bake_hash_mix(h, uint64_t(font->packedfile->size));
      h = bake_hash_mix(h, uint64_t(reinterpret_cast<uintptr_t>(font->packedfile->data)));
    }
  }
  return h;
}

/* -------------------------------------------------------------------- */
/** \name Runtime state
 *
 * A Text Decal's raster is not a row bake, so its state lives in its own map (see
 * `paint_layers_text_decal_intern.hh`) rather than inside #BakeMaterialRuntime. The lock is held
 * around the entry only, never across a raster: the map can rehash while another thread reads it,
 * so no pointer into it outlives the lock.
 * \{ */

Map<uint32_t, TextDecalRuntime> &text_decal_runtime()
{
  static Map<uint32_t, TextDecalRuntime> map;
  return map;
}

std::mutex &text_decal_runtime_mutex()
{
  static std::mutex mutex;
  return mutex;
}

void text_decal_runtime_free(Material &ma)
{
  std::lock_guard lock(text_decal_runtime_mutex());
  const TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(ma.id.session_uid);
  /* Only the owning material may drop the entry: a same-uid old ID freed during a memfile undo
   * must leave the re-read ID's state in place. */
  if (runtime != nullptr && runtime->owner == &ma) {
    text_decal_runtime().remove(ma.id.session_uid);
  }
}

void text_decal_runtime_owner_transfer(Material &dst, Material &src)
{
  std::lock_guard lock(text_decal_runtime_mutex());
  TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(src.id.session_uid);
  if (runtime != nullptr && runtime->owner == &src) {
    runtime->owner = &dst;
  }
}

/** The state of \a marker, or a fresh one. False when another material owns the runtime entry. */
static bool text_decal_state_get(const Material &ma, const bUUID &marker, TextDecalEntry &r_entry)
{
  std::lock_guard lock(text_decal_runtime_mutex());
  TextDecalRuntime &runtime = text_decal_runtime().lookup_or_add(ma.id.session_uid,
                                                                TextDecalRuntime{});
  if (runtime.owner == nullptr) {
    runtime.owner = &ma;
  }
  if (runtime.owner != &ma) {
    return false;
  }
  for (const TextDecalEntry &entry : runtime.entries) {
    if (BLI_uuid_equal(entry.row, marker)) {
      r_entry = entry;
      return true;
    }
  }
  r_entry = TextDecalEntry{};
  r_entry.row = marker;
  return true;
}

static void text_decal_state_set(const Material &ma, const TextDecalEntry &state)
{
  std::lock_guard lock(text_decal_runtime_mutex());
  TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime == nullptr || runtime->owner != &ma) {
    return;
  }
  for (TextDecalEntry &entry : runtime->entries) {
    if (BLI_uuid_equal(entry.row, state.row)) {
      entry = state;
      return;
    }
  }
  runtime->entries.append(state);
}

/** Drop the states of rows not in \a live. True when one was dropped. */
static bool text_decal_state_prune(const Material &ma, const Span<bUUID> live)
{
  std::lock_guard lock(text_decal_runtime_mutex());
  TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime == nullptr || runtime->owner != &ma) {
    return false;
  }
  bool removed = false;
  for (int i = int(runtime->entries.size()) - 1; i >= 0; i--) {
    const bUUID &row = runtime->entries[i].row;
    bool found = false;
    for (const bUUID &marker : live) {
      if (BLI_uuid_equal(marker, row)) {
        found = true;
        break;
      }
    }
    if (!found) {
      runtime->entries.remove_and_reorder(i);
      removed = true;
    }
  }
  return removed;
}

/** \} */

/**
 * Write the raster's coverage into \a image. An effect's map stores the text as alpha over white, so
 * it reads as the mask of whatever the channels lay; a mask item's map stores the coverage as an
 * opaque grey, the convention of every coverage map. A null raster is an empty text: every texel is
 * transparent, which matters because a missing coverage map reads as full coverage.
 */
static void text_decal_coverage_write(Image &image,
                                      const bool is_mask,
                                      const ImBuf *raster)
{
  bake_write_image_rows(
      image, false, nullptr, 0, [&](const int y, const int width, float *r_row) {
        const float *row_pixels = raster != nullptr ?
                                      raster->float_data() +
                                          int64_t(y) * raster->x * 4 :
                                      nullptr;
        for (const int x : IndexRange(width)) {
          float alpha = 0.0f;
          if (row_pixels != nullptr) {
            alpha = clamp_f(row_pixels[x * 4 + 3], 0.0f, 1.0f);
          }
          float *pixel = r_row + x * 4;
          if (is_mask) {
            pixel[0] = pixel[1] = pixel[2] = alpha;
            pixel[3] = 1.0f;
          }
          else {
            pixel[0] = pixel[1] = pixel[2] = 1.0f;
            pixel[3] = alpha;
          }
        }
      });
}

/**
 * The map the raster is written into, at \a side: the effect's own coverage map, or a mask item's
 * Base Color map. \a existing is the current one (null when there is none); a map of another size
 * is scaled in place, and one that cannot be scaled is replaced. Null when the map could not be
 * made.
 */
static Image *text_decal_image_ensure(Main &bmain,
                                      Image *existing,
                                      const char *name,
                                      const int side,
                                      const bool is_color,
                                      bool &r_created)
{
  if (existing != nullptr) {
    int width = 0, height = 0;
    BKE_image_get_size(existing, nullptr, &width, &height);
    if (width == side && height == side) {
      return existing;
    }
    /* The payload's size changed. Resize in place rather than replace: the generated tree still
     * names this image until the next regeneration, and replacing it would leave the tree pointing
     * at an orphan (or force a free the shader may still read). */
    if (BKE_image_scale(existing, side, side, nullptr)) {
      return existing;
    }
    /* No buffer to scale (a map whose pixels were never loaded): make a new one below. */
  }
  const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *created = paint_layers_map_create(
      bmain, name, side, is_color, transparent, /*tiled=*/false, /*is_float=*/false);
  r_created = created != nullptr;
  return created;
}

/** The effect's coverage map at \a side, made when missing. The payload owns the born user. */
static Image *text_decal_coverage_ensure(Main &bmain,
                                         MaterialPaintLayer &row,
                                         const int side,
                                         bool &r_created)
{
  MaterialPaintLayerText &payload = *row.text_decal;
  char name[192];
  SNPRINTF(name, "%s Coverage", row.name[0] != '\0' ? row.name : "Text Decal");
  bool created = false;
  Image *image = text_decal_image_ensure(bmain, payload.coverage, name, side, false, created);
  if (created) {
    if (payload.coverage != nullptr) {
      /* The unscalable old map is left as an orphan, as a removed row's map is. */
      id_us_min(&payload.coverage->id);
    }
    payload.coverage = image;
    r_created = true;
  }
  return image;
}

/**
 * The map of a mask item's Base Color record at \a side, made when missing. A mask item's map is a
 * number, not a colour.
 */
static Image *text_decal_mask_map_ensure(Main &bmain,
                                         Material &ma,
                                         MaterialPaintLayer &row,
                                         const int side,
                                         bool &r_created)
{
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(row, channel);
  char name[192];
  SNPRINTF(name,
           "%s %s",
           row.name[0] != '\0' ? row.name : "Layer",
           BKE_paint_material_channel_info(channel).ui_name);
  bool created = false;
  Image *image = text_decal_image_ensure(
      bmain, record != nullptr ? record->image : nullptr, name, side, false, created);
  if (created) {
    if (!BKE_paint_layers_channel_set_image(ma, &row, channel, image)) {
      BKE_id_free(&bmain, &image->id);
      return nullptr;
    }
    /* The channel took its own user; the one the image was born with goes. */
    id_us_min(&image->id);
    r_created = true;
  }
  return image;
}

/**
 * Whether the mask item \a row has a live Base Color record to write its grey into. A mask item
 * is the only Text Decal that writes a channel: an effect keeps the user's channel values and
 * textures and writes only its own coverage map.
 */
static bool text_decal_mask_channel_live(const MaterialPaintLayer &row)
{
  return paint_layer_channel_live(paint_layer_channel_find(row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

/**
 * Where a Text Decal raster lands. The effect owns a coverage map; a mask item writes the grey into
 * its Base Color record. A third target only has to add another implementation, not another branch
 * in the refresh loop.
 */
class TextDecalSink {
 public:
  virtual ~TextDecalSink() = default;
  /** Whether the row has a live place to write into at all. */
  virtual bool has_target(const MaterialPaintLayer &row) const = 0;
  /** The map at \a side, made when missing; \a r_created says a new map was made. */
  virtual Image *ensure(Main &bmain,
                        Material &ma,
                        MaterialPaintLayer &row,
                        int side,
                        bool &r_created) const = 0;
  /** Write the raster's coverage into the map; a null raster is an empty text. */
  virtual void write(Image &image, const ImBuf *raster) const = 0;
};

/** A Text Decal effect: the text is the mask of the whole effect, stored as alpha over white. */
class TextDecalEffectSink : public TextDecalSink {
 public:
  bool has_target(const MaterialPaintLayer & /*row*/) const override
  {
    return true;
  }
  Image *ensure(Main &bmain,
                Material & /*ma*/,
                MaterialPaintLayer &row,
                const int side,
                bool &r_created) const override
  {
    return text_decal_coverage_ensure(bmain, row, side, r_created);
  }
  void write(Image &image, const ImBuf *raster) const override
  {
    text_decal_coverage_write(image, false, raster);
  }
};

/** A mask item: the coverage is the opaque grey of the item's Base Color map. */
class TextDecalMaskSink : public TextDecalSink {
 public:
  bool has_target(const MaterialPaintLayer &row) const override
  {
    return text_decal_mask_channel_live(row);
  }
  Image *ensure(Main &bmain,
                Material &ma,
                MaterialPaintLayer &row,
                const int side,
                bool &r_created) const override
  {
    return text_decal_mask_map_ensure(bmain, ma, row, side, r_created);
  }
  void write(Image &image, const ImBuf *raster) const override
  {
    text_decal_coverage_write(image, true, raster);
  }
};

static const TextDecalEffectSink text_decal_effect_sink;
static const TextDecalMaskSink text_decal_mask_sink;

static const TextDecalSink &text_decal_sink_for(const MaterialPaintLayer &row)
{
  if (row.role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    return text_decal_mask_sink;
  }
  return text_decal_effect_sink;
}

/**
 * The object the raster reads \a payload's content from. The evaluated copy carries animation,
 * drivers, shape keys and modifiers; a live text edit is the exception, because the edit buffer
 * lives on the original object's #Curve.
 */
static Object *text_decal_source_object(const MaterialPaintLayerText &payload,
                                        const Depsgraph *depsgraph)
{
  Object *original = payload.text_object;
  if (original == nullptr || depsgraph == nullptr) {
    return original;
  }
  const Curve *curve = id_cast<const Curve *>(original->data);
  if (curve != nullptr && curve->editfont != nullptr) {
    return original;
  }
  Object *evaluated = DEG_get_evaluated<Object>(depsgraph, original);
  return (evaluated != nullptr && evaluated->type == OB_FONT) ? evaluated : original;
}

/**
 * The public vfont parameters of a Text Decal payload. The DNA enum stops here: the raster module
 * takes its own fit vocabulary, so it never depends on the Paint Layers format.
 */
static VFontRenderFit text_decal_render_fit(const int8_t fit)
{
  return fit == MA_PAINT_LAYER_TEXT_FIT_STRETCH ? VFontRenderFit::Stretch :
                                                  VFontRenderFit::Contain;
}

/** The timer's identity: its address, shared by every refresh of the process. */
static const char text_decal_wake_anchor = 0;

static uintptr_t text_decal_wake_uuid()
{
  return reinterpret_cast<uintptr_t>(&text_decal_wake_anchor);
}

/**
 * Fires once after the typing pause. Blender does not update by itself when typing stops, so the
 * wake tags the Text objects still being edited; the scene update that follows runs the refresh,
 * which finds the hash stable for long enough and rasterizes. A material whose decal is not held
 * back by a live edit has nothing due and is left alone.
 */
static double text_decal_wake_tick(uintptr_t /*uuid*/, void * /*user_data*/)
{
  if (G_MAIN == nullptr) {
    return -1.0;
  }
  Vector<Object *> due;
  {
    std::lock_guard lock(text_decal_runtime_mutex());
    for (TextDecalRuntime &runtime : text_decal_runtime().values()) {
      if (runtime.owner == nullptr || runtime.present != 1) {
        continue;
      }
      for (Object *object : runtime.editing_sources) {
        const Curve *curve = object != nullptr ? id_cast<const Curve *>(object->data) : nullptr;
        if (curve != nullptr && curve->editfont != nullptr) {
          due.append(object);
        }
      }
    }
  }
  for (Object *object : due) {
    DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  }
  return -1.0;
}

/** (Re)arm the wake-up \a wait seconds from now; a later keystroke re-arms it again. */
static void text_decal_wake_arm(const double wait)
{
  if (BLI_timer_is_registered(text_decal_wake_uuid())) {
    BLI_timer_unregister(text_decal_wake_uuid());
  }
  BLI_timer_register(text_decal_wake_uuid(), text_decal_wake_tick, nullptr, nullptr, wait, false);
}

/** Whether the Text object \a payload reads is being edited (its edit buffer holds the live text). */
static bool text_decal_source_editing(const MaterialPaintLayerText &payload)
{
  const Object *original = payload.text_object;
  const Curve *curve = (original != nullptr) ? id_cast<const Curve *>(original->data) : nullptr;
  return curve != nullptr && curve->editfont != nullptr;
}

void BKE_paint_layers_text_decal_refresh(Main &bmain,
                                         Material &ma,
                                         const Depsgraph *depsgraph)
{
  if (!BKE_paint_layers_is_layered(ma) || !ID_IS_EDITABLE(&ma.id) ||
      ID_IS_OVERRIDE_LIBRARY(&ma.id))
  {
    return;
  }

  /* A material that answered "no Text Decal" once costs a cached read on every update; only an
   * add, a copy or an undo clears the answer (see #BKE_paint_layers_text_decal_invalidate). */
  {
    std::lock_guard lock(text_decal_runtime_mutex());
    TextDecalRuntime &runtime = text_decal_runtime().lookup_or_add(ma.id.session_uid,
                                                                  TextDecalRuntime{});
    if (runtime.owner == nullptr) {
      runtime.owner = &ma;
    }
    if (runtime.owner != &ma) {
      return;
    }
    if (runtime.present == 0) {
      return;
    }
  }

  /* A Text Decal exists as an effect of a row (a folder included) or as an item of its mask. The
   * rows are collected first: the raster below edits channel records, which the walk must not see
   * move. */
  Vector<MaterialPaintLayer *> rows;
  bool has_text_decal = false;
  const auto collect = [&](MaterialPaintLayer &row) {
    if (row.source != MA_PAINT_LAYER_SOURCE_TEXT || row.text_decal == nullptr) {
      return;
    }
    has_text_decal = true;
    if (row.text_decal->text_object != nullptr) {
      rows.append(&row);
    }
  };
  BKE_paint_layers_foreach(ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) {
    for (MaterialPaintLayer &effect : layer.effects) {
      collect(effect);
    }
    for (MaterialPaintLayer &item : layer.mask_stack) {
      collect(item);
    }
    return true;
  });

  {
    std::lock_guard lock(text_decal_runtime_mutex());
    TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(ma.id.session_uid);
    if (runtime == nullptr || runtime->owner != &ma) {
      return;
    }
    runtime->present = has_text_decal ? 1 : 0;
    /* The wake timer tags the objects whose live edit holds a raster back, not every material
     * with a decal: an object outside Edit Mode cannot produce a new hash on its own. */
    runtime->editing_sources.clear();
    for (const MaterialPaintLayer *row : rows) {
      if (text_decal_source_editing(*row->text_decal)) {
        runtime->editing_sources.append(row->text_decal->text_object);
      }
    }
  }
  if (!has_text_decal) {
    Vector<bUUID> none;
    if (text_decal_state_prune(ma, none)) {
      ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
      DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
    }
    return;
  }

  Vector<bUUID> live;
  bool structure_changed = false;
  /* The nearest time a row held back by a live text edit comes due, or negative for none. */
  double next_wait = -1.0;
  for (MaterialPaintLayer *row : rows) {
    live.append(row->marker);
    const MaterialPaintLayerText &payload = *row->text_decal;
    const TextDecalSink &sink = text_decal_sink_for(*row);
    /* A mask item without a live Base Color record has nowhere to write. */
    if (!sink.has_target(*row)) {
      continue;
    }
    const Object &text_object = *text_decal_source_object(payload, depsgraph);
    /* Only what the raster depends on: a channel value, colour, texture or blend of an effect is
     * no input of it, so editing those never reaches this file's work. */
    const uint64_t hash = text_decal_hash(payload, text_object);
    TextDecalEntry state;
    if (!text_decal_state_get(ma, row->marker, state)) {
      /* Another material owns the runtime entry: this row has no state to write. The scan still
       * finishes so rows that are gone are pruned. */
      continue;
    }
    /* An effect whose coverage map is gone (a copy, an undo) has to be rasterized again. */
    if (state.hash == hash && state.has_raster &&
        (row->role == MA_PAINT_LAYER_ROLE_MASK_ITEM || payload.coverage != nullptr)) {
      continue;
    }

    /* While the text is being typed, wait for the edit buffer to hold still. A fresh state (hash
     * zero: the first bake, a re-bake, an undo) renders at once, and so does leaving Edit Mode,
     * where the buffer is gone and nothing more can change. */
    if (state.hash != 0 && text_decal_source_editing(payload)) {
      const double now = BLI_time_now_seconds();
      if (state.observed_hash != hash) {
        state.observed_hash = hash;
        state.observed_time = now;
        text_decal_state_set(ma, state);
      }
      const double wait = state.observed_time + TEXT_DECAL_EDIT_DELAY_SECONDS - now;
      if (wait > 0.0) {
        next_wait = (next_wait < 0.0) ? wait : std::min(next_wait, wait);
        continue;
      }
    }

    const int side = BKE_paint_layers_text_decal_size_get(payload);
    state.hash = hash;
    state.observed_hash = hash;
    state.has_raster = false;

#if PBR_LAYERS_DIAGNOSTICS
    const double start = BLI_time_now_seconds();
#endif
    const VFontRenderParams render_params = {
        side, payload.padding, text_decal_render_fit(payload.fit)};
    ImBuf *raster = BKE_vfont_object_render(text_object, render_params);
    if (raster == nullptr) {
      /* No text, no font: the decal covers nothing. A missing coverage map would read as full
       * coverage and flood the surface with the channel values. */
      PBR_LAYERS_LOG("[paint-layers text decal] row \"%s\": no raster (empty text or no font)\n",
                     row->name);
    }
    bool created = false;
    Image *image = sink.ensure(bmain, ma, *row, side, created);
    structure_changed = structure_changed || created;
    if (image != nullptr) {
      /* The whole buffer is written, transparent texels included, so no content reset is needed. */
      sink.write(*image, raster);
      state.has_raster = true;
    }
    if (raster != nullptr) {
      IMB_freeImBuf(raster);
    }
    text_decal_state_set(ma, state);
    PBR_LAYERS_LOG("[paint-layers text decal] row \"%s\" size=%d time=%.1f ms\n",
                   row->name,
                   side,
                   (BLI_time_now_seconds() - start) * 1000.0);
  }

  /* A row that stopped being a Text Decal drops its state; the map itself is the row's, released
   * by the row's own free path. */
  if (text_decal_state_prune(ma, live)) {
    structure_changed = true;
  }
  if (structure_changed) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
    DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  }
  if (next_wait >= 0.0) {
    text_decal_wake_arm(next_wait);
  }
}

void BKE_paint_layers_text_decal_refresh_tagged(Main &bmain, const Depsgraph &depsgraph)
{
  /* BLF is global state: only the main thread may rasterize. Render workers reach the scene update
   * as well, and the next main-thread update picks the work up. */
  if (!BLI_thread_is_main()) {
    return;
  }
  for (Material &ma : bmain.materials) {
    if (BKE_paint_layers_is_layered(ma)) {
      BKE_paint_layers_text_decal_refresh(bmain, ma, &depsgraph);
    }
  }
}

void BKE_paint_layers_text_decal_invalidate(Material &ma)
{
  std::lock_guard lock(text_decal_runtime_mutex());
  TextDecalRuntime *runtime = text_decal_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime == nullptr || runtime->owner != &ma) {
    return;
  }
  for (TextDecalEntry &entry : runtime->entries) {
    entry.hash = 0;
    entry.has_raster = false;
  }
  /* The stack may have gained or lost a Text Decal under the undo step; the cached answer is not
   * trustworthy any more. */
  runtime->present = -1;
}

}  // namespace blender
