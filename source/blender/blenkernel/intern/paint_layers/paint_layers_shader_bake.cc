/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The shader-bake's pure half: the cache of the node-group kinds' answers, keyed on the
 * definition and the values, standing while neither moved. The backend (the editors, which hold
 * the GPU context and the material compiler) renders into the entries; the composite, the bake
 * and the effect cache read them as maps.
 *
 * The key is the row's identity in the evaluator's own terms -- the material, the row, the tile --
 * folded with the definition hash (#BKE_paint_layers_node_group_definition_hash) and the row's
 * packed parameters. The first pass answers one map per row: a source tree's `Result` (a tree
 * with per-channel outputs is the per-channel refinement, v2), a filter's `Result` over its
 * input, a generator's `Fac`. The entry records the hash it was rendered from; a reader sees
 * stale and a due-check sees due when the two disagree, the way the bake maps' claim hashes work.
 * The images are generated, float-buffered, unsaved: a reload re-bakes, like a preview.
 */

#include <memory>
#include <mutex>

#include "BLI_hash.hh"
#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_threads.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_node_group.hh"
#include "BKE_paint_layers_shader_bake.hh"
#include "BKE_paint_layers_tile.hh"

#include "DNA_ID.h"
#include "DNA_color_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name The hooks
 * \{ */

static PaintLayerShaderBakeHooks shader_bake_hooks;

void BKE_paint_layers_shader_bake_hooks_set(const PaintLayerShaderBakeHooks &hooks)
{
  shader_bake_hooks = hooks;
}

const PaintLayerShaderBakeHooks *BKE_paint_layers_shader_bake_hooks_get()
{
  return &shader_bake_hooks;
}

Main *BKE_paint_layers_shader_bake_main()
{
  return shader_bake_hooks.main;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The key
 * \{ */

struct ShaderBakeKey {
  uint32_t material_uid = 0;
  /** The row's marker, hashed: the marker is the row's identity across reorder and copy. */
  uint64_t row_marker = 0;
  int tile_number = 0;
  /**
   * The paint channel the entry answers: -1 on the one-answer contract (every channel reads the
   * same `Result` map), the channel itself for a per-channel source. Two channels of one row are
   * two entries, never one map doubling for the other.
   */
  int channel = 0;
  /** What the definition is (the tree's interface, the chain's members) and the row's kind. */
  uint64_t definition_hash = 0;
  /** The row's stored values, packed the way the composite packs them. */
  uint64_t params_hash = 0;

  uint64_t hash() const
  {
    return get_default_hash(material_uid,
                            row_marker,
                            uint64_t(tile_number),
                            uint64_t(channel),
                            definition_hash,
                            params_hash);
  }
  bool operator==(const ShaderBakeKey &other) const
  {
    return material_uid == other.material_uid && row_marker == other.row_marker &&
           tile_number == other.tile_number && channel == other.channel &&
           definition_hash == other.definition_hash && params_hash == other.params_hash;
  }
};

/** Whether \a row's kind is one this cache answers. */
static bool shader_bake_kind_of(const MaterialPaintLayer &row)
{
  /* A node-group filter is deliberately not here: it is a correction computed by the effect cache's
   * kernel, which asks the hooks for the pixels directly (#paint_layer_effect_kernel_apply), so it
   * owns both its cache and its input. Answering it from this cache would have no `Input` to feed
   * the tree -- the request carries none, because the filter's content is the effect cache's
   * business. The layer and the generator read whole maps here; their inputs are the UV and the
   * placement chain, not a content buffer. */
  return row.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP ||
         (row.source == MA_PAINT_LAYER_SOURCE_GENERATOR &&
          row.kind_subtype == MA_PAINT_LAYER_GENERATOR_NODE_GROUP);
}

static ShaderBakeKey shader_bake_key(const Material &ma,
                                     const MaterialPaintLayer &row,
                                     const int tile_number,
                                     const int channel)
{
  ShaderBakeKey key;
  key.material_uid = ma.id.session_uid;
  /* The marker's whole 16 bytes: hashing only the time half would collide two markers that share
   * it, and the key is the row's identity -- the whole marker, not its timestamp. */
  const bUUID &marker = row.marker;
  const uint64_t marker_low = uint64_t(marker.time_low) |
                              (uint64_t(marker.time_mid) << 32) |
                              (uint64_t(marker.time_hi_and_version) << 48);
  const uint64_t marker_high = uint64_t(marker.clock_seq_hi_and_reserved) |
                               (uint64_t(marker.clock_seq_low) << 8) |
                               (uint64_t(marker.node[0]) << 16) |
                               (uint64_t(marker.node[1]) << 24) |
                               (uint64_t(marker.node[2]) << 32) |
                               (uint64_t(marker.node[3]) << 40) |
                               (uint64_t(marker.node[4]) << 48) |
                               (uint64_t(marker.node[5]) << 56);
  key.row_marker = get_default_hash(marker_low, marker_high);
  /* The tile is resolved the way the GPU effect cache resolves its own key, so an unset tile
   * (#PAINT_TILE_NUMBER_AUTO) and the base tile it falls back to ask one entry instead of two
   * that would hold the same answer apart. */
  key.tile_number = BKE_paint_layers_tile_resolve(tile_number, nullptr, nullptr);
  key.channel = channel;
  key.definition_hash = BKE_paint_layers_node_group_definition_hash(row);
  /* The values: every stored parameter, packed the way the composite packs them. A color or a
   * vector occupies its three slots here too, so the hash is the packing's hash. */
  uint64_t params = 0;
  if (const PaintLayerKindInfo *kind = paint_layer_kind_of(row)) {
    for (const int i : IndexRange(kind->params_num)) {
      switch (kind->params[i].type) {
        case PaintLayerKindParamType::Color:
        case PaintLayerKindParamType::Vector: {
          float packed[4] = {0.0f, 0.0f, 0.0f, 0.0f};
          if (kind->params[i].type == PaintLayerKindParamType::Color) {
            paint_layer_kind_param_color_get(row, i, packed);
          }
          else {
            paint_layer_kind_param_vector_get(row, i, packed);
          }
          params = get_default_hash(params, packed[0], packed[1], packed[2]);
          break;
        }
        case PaintLayerKindParamType::Ramp:
          if (const ColorBand *band = BKE_paint_layers_ramp_get(row)) {
            params = get_default_hash(params, paint_layer_ramp_hash(*band));
          }
          break;
        case PaintLayerKindParamType::Curves:
          if (const CurveMapping *curves = BKE_paint_layers_curves_get(row)) {
            params = get_default_hash(params, paint_layer_curves_hash(*curves));
          }
          break;
        default:
          params = get_default_hash(params, paint_layer_kind_param_get(row, i));
          break;
      }
    }
  }
  /* A node-group generator's answer runs the ramp tail after the tree: the per-record value ramps
   * and their two ends are part of the answer the schema's own Ramp slot does not carry, so they
   * fold beside it -- an edit of a scalar channel's ramp or ends retires the entries with the
   * band's. The records are the same channels the ask's list reads, so an added or removed one
   * moves the keys themselves and needs no fold here. */
  if (row.source == MA_PAINT_LAYER_SOURCE_GENERATOR &&
      row.kind_subtype == MA_PAINT_LAYER_GENERATOR_NODE_GROUP)
  {
    for (const int i : IndexRange(row.channels_num)) {
      const MaterialPaintLayerChannel &record = row.channels[i];
      params = get_default_hash(params, record.value[0], record.value[1]);
      if (record.gradient_ramp != nullptr) {
        params = get_default_hash(params, paint_layer_ramp_hash(*record.gradient_ramp));
      }
    }
  }
  key.params_hash = params;
  return key;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The entries
 * \{ */

struct ShaderBakeEntry {
  /** The identity the pixels were rendered from; a different hash is a stale entry. */
  ShaderBakeKey key;
  /** Another size is another texel space: an entry stands at its own size only. */
  int width = 0;
  int height = 0;
  /**
   * Whether the image holds the answer for #key. An entry made by a reader (an empty generated
   * map it can lay before the render lands) stays false, so the due-check asks for the render
   * that fills it; a successful #BKE_paint_layers_shader_bake_compute sets it.
   */
  bool rendered = false;
  /** The generated image the answer is written into. Owned by the cache for the session. */
  Image *image = nullptr;
};

static Map<ShaderBakeKey, std::unique_ptr<ShaderBakeEntry>> &shader_bake_cache()
{
  static Map<ShaderBakeKey, std::unique_ptr<ShaderBakeEntry>> cache;
  return cache;
}

/**
 * The row half of the key: one row's channel on one tile, whatever the definition and the values
 * spell. The stale policy (the row's last answer stands until the render replaces it) and the
 * eviction (the new identity supersedes the row's older ones) are questions about a row, not
 * about the cache -- this index hands the row's own keys, so neither walk scans every entry the
 * cache holds on the read path. The cache's own shadow: kept in step under the same mutex, never
 * ahead of it and never behind.
 */
struct ShaderBakeRowId {
  uint32_t material_uid = 0;
  uint64_t row_marker = 0;
  int tile_number = 0;
  int channel = 0;

  uint64_t hash() const
  {
    return get_default_hash(material_uid, row_marker, uint64_t(tile_number), uint64_t(channel));
  }
  bool operator==(const ShaderBakeRowId &other) const
  {
    return material_uid == other.material_uid && row_marker == other.row_marker &&
           tile_number == other.tile_number && channel == other.channel;
  }
};

static Map<ShaderBakeRowId, Vector<ShaderBakeKey>> &shader_bake_row_index()
{
  static Map<ShaderBakeRowId, Vector<ShaderBakeKey>> index;
  return index;
}

static ShaderBakeRowId shader_bake_row_id(const ShaderBakeKey &key)
{
  ShaderBakeRowId row_id;
  row_id.material_uid = key.material_uid;
  row_id.row_marker = key.row_marker;
  row_id.tile_number = key.tile_number;
  row_id.channel = key.channel;
  return row_id;
}

/** Track \a key under its row. The caller holds the cache mutex. */
static void shader_bake_row_index_add(const ShaderBakeKey &key)
{
  shader_bake_row_index().lookup_or_add_default(shader_bake_row_id(key)).append(key);
}

/** Drop \a key from the cache and from its row, and the row once its last key is gone. The caller
 * holds the cache mutex. */
static void shader_bake_cache_remove(const ShaderBakeKey &key)
{
  shader_bake_cache().remove(key);
  const ShaderBakeRowId row_id = shader_bake_row_id(key);
  if (Vector<ShaderBakeKey> *row_keys = shader_bake_row_index().lookup_ptr(row_id)) {
    row_keys->remove_first_occurrence_and_reorder(key);
    if (row_keys->is_empty()) {
      shader_bake_row_index().remove(row_id);
    }
  }
}

static std::mutex &shader_bake_cache_mutex()
{
  static std::mutex mutex;
  return mutex;
}

static Image *shader_bake_image_ensure(Main &bmain, const int width, const int height)
{
  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *image = BKE_image_add_generated(&bmain,
                                         unsigned(width),
                                         unsigned(height),
                                         "PL Shader Bake",
                                         32,
                                         /*floatbuf=*/true,
                                         IMA_GENTYPE_BLANK,
                                         black,
                                         /*stereo3d=*/false,
                                         /*is_data=*/true,
                                         /*tiled=*/false);
  if (image != nullptr) {
    /* Session-local: the pixels are a cache, not data, and a reload re-bakes. The runtime tag is
     * what keeps the datablock out of the .blend and of the undo snapshots (writefile skips
     * #ID_TAG_RUNTIME); #ID_TAG_LOCALIZED means "a localized library datablock" and writes. */
    image->id.tag |= ID_TAG_RUNTIME;
  }
  return image;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The reads
 * \{ */

/**
 * The channels a row answers, in channel order: -1 on the one-answer contract (every channel
 * reads the same `Result` map), the tree's named channels on the per-channel one.
 */
static Vector<int> fed_shader_bake_channels(const bool multi,
                                            const bool per_channel[PAINT_MATERIAL_CHANNEL_NUM])
{
  Vector<int> channels;
  if (!multi) {
    /* The one-answer contract: `Result` is laid into every channel, and the backend reads the
     * negative channel as exactly that. Zero would name Base Color alone and miss `Result`. */
    channels.append(-1);
    return channels;
  }
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (per_channel[i]) {
      channels.append(i);
    }
  }
  return channels;
}

/**
 * The channels a node-group generator row answers: its own live channel records, in channel
 * order, one map per channel -- the pattern emission is rendered once and each channel reads the
 * ramp tail keyed to it, the way the describe keys one bake map per channel. The predicate is the
 * composite's own participation rule for a #PAINT_LAYER_KIND_GENERATES row: a live record is the
 * whole condition, and Normal is out whatever the record says (the registry's
 * #PAINT_LAYER_GENERATOR_NO_NORMAL, which the channel-add rule already keeps off the row).
 */
static Vector<int> generator_shader_bake_channels(const Material &ma, const MaterialPaintLayer &row)
{
  Vector<int> channels;
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(row, channel);
    if (paint_layer_channel_contributes(ma, row, record)) {
      channels.append(channel);
    }
  }
  return channels;
}

/**
 * The channels \a row's shader-bake answers, from the kind: a node-group generator row answers
 * its record channels, a source row the one-answer or per-channel contract its tree spells.
 */
static Vector<int> shader_bake_channels(const Material &ma, const MaterialPaintLayer &row)
{
  if (row.source == MA_PAINT_LAYER_SOURCE_GENERATOR &&
      row.kind_subtype == MA_PAINT_LAYER_GENERATOR_NODE_GROUP)
  {
    return generator_shader_bake_channels(ma, row);
  }
  bool per_channel[PAINT_MATERIAL_CHANNEL_NUM] = {};
  const bool multi =
      row.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP &&
      BKE_paint_layers_node_group_source_channels(
          row.kind_tree, MutableSpan<bool>(per_channel, PAINT_MATERIAL_CHANNEL_NUM));
  return fed_shader_bake_channels(multi, per_channel);
}

/**
 * The generator's ramp context for one channel, as the compute fills it into the ask: the band
 * the channel's tail reads and the two ends the null-band fallback takes, exactly the answer the
 * CPU composite's generator payload carries (#composite_generator_describe). A colour channel
 * reads the row's own band -- the Ramp parameter of the dynamic schema kept it on the row -- the
 * same band #generator_ramp_ensure copies into the graph's ColorRamp node; a scalar channel reads
 * its record's value ramp and the encoded ends, whose stops and ends are both in the channel's
 * stored scale. A null band leaves the backend the linear pass, which is the fallback
 * #BKE_paint_material_generator_sample_coordinate lays when a channel has no band: the raw factor
 * as a grey, from \a r_scalar_start at 0 to \a r_scalar_end at 1.
 */
static void generator_shader_bake_ramp_fill(const MaterialPaintLayer &row,
                                            const int channel,
                                            const ColorBand *&r_ramp,
                                            float &r_scalar_start,
                                            float &r_scalar_end)
{
  r_ramp = nullptr;
  r_scalar_start = 0.0f;
  r_scalar_end = 1.0f;
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  if (info.is_color) {
    r_ramp = BKE_paint_layers_ramp_get(row);
    return;
  }
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(row, channel);
  if (record == nullptr) {
    return;
  }
  /* The ends are authored as relief and worked in storage: the describe encodes them once, so
   * the bake lays the stored value the channel's socket expects. */
  r_scalar_start = BKE_paint_material_channel_value_encode(eMaterialPaintChannel(channel),
                                                           record->value[0]);
  r_scalar_end = BKE_paint_material_channel_value_encode(eMaterialPaintChannel(channel),
                                                         record->value[1]);
  r_ramp = record->gradient_ramp;
}

Image *BKE_paint_layers_shader_bake_image_get(Main &bmain,
                                              const Material &ma,
                                              const MaterialPaintLayer &row,
                                              const int channel,
                                              const PaintLayerTileContext &tile,
                                              const int width,
                                              const int height)
{
  if (!shader_bake_kind_of(row) || shader_bake_hooks.render == nullptr || row.kind_tree == nullptr ||
      width <= 0 || height <= 0)
  {
    return nullptr;
  }
  /* Thread contract: a miss creates a datablock in \a bmain (#BKE_image_add_generated below),
   * which is main-thread business. Every caller today keeps it -- the composite and the sweep run
   * on the main thread, and a bake worker reaches the node-group answers through the effect cache,
   * whose kernel goes through the hooks without touching this cache. A caller that breaks the
   * contract is refused rather than corrupting the Main: the debug build also fires here. */
  BLI_assert(BLI_thread_is_main());
  if (!BLI_thread_is_main()) {
    return nullptr;
  }
  const ShaderBakeKey key = shader_bake_key(ma, row, tile.tile_number, channel);
  Image *stale_image = nullptr;
  {
    std::lock_guard lock(shader_bake_cache_mutex());
    if (const std::unique_ptr<ShaderBakeEntry> *entry = shader_bake_cache().lookup_ptr(key)) {
      if ((*entry)->width == width && (*entry)->height == height) {
        return (*entry)->image;
      }
      /* Another size is another texel space: the entry stands for the old one no longer. */
      if (Image *old = (*entry)->image) {
        id_us_min(&old->id);
      }
      shader_bake_cache_remove(key);
    }
    /* The identity moved (an edit of the definition or of the values): the row's previous answer
     * still stands, and a reader lays it while the due-check brings the render -- the stale
     * policy the header promises, the way the source graph's cache serves its last bake. The row
     * index hands the row's own entries, not every entry the cache holds. */
    if (const Vector<ShaderBakeKey> *row_keys = shader_bake_row_index().lookup_ptr(
            shader_bake_row_id(key)))
    {
      for (const ShaderBakeKey &old_key : *row_keys) {
        const std::unique_ptr<ShaderBakeEntry> *entry = shader_bake_cache().lookup_ptr(old_key);
        if (entry == nullptr) {
          continue;
        }
        if ((*entry)->width == width && (*entry)->height == height && (*entry)->image != nullptr)
        {
          stale_image = (*entry)->image;
          break;
        }
      }
    }
  }
  Image *image = (stale_image != nullptr) ? stale_image :
                                            shader_bake_image_ensure(bmain, width, height);
  if (image == nullptr) {
    return nullptr;
  }
  {
    std::lock_guard lock(shader_bake_cache_mutex());
    /* The race the two lock sections leave: another ask of the same identity may have landed its
     * own entry while this one was outside the lock making the image. The winner's entry stands
     * -- the overwrite would drop it with its image's user left hanging -- so it answers, and
     * the image this pass made goes back: a fresh one turns an orphan the file purge takes, a
     * shared stale one keeps its owning entry's user (this pass never plussed it). */
    if (const std::unique_ptr<ShaderBakeEntry> *existing = shader_bake_cache().lookup_ptr(key)) {
      if ((*existing)->width == width && (*existing)->height == height) {
        if (stale_image == nullptr && image != (*existing)->image) {
          id_us_min(&image->id);
        }
        return (*existing)->image;
      }
      /* A different size is another texel space: the same rule the first section applies. */
      if (Image *old = (*existing)->image) {
        id_us_min(&old->id);
      }
      shader_bake_cache_remove(key);
    }
    if (stale_image != nullptr) {
      /* The new entry takes the image's user; the old entries below drop theirs. */
      id_us_plus(&image->id);
    }
    /* The new identity supersedes the row's older ones on this tile *and channel*: without this
     * the map grows one entry per edit. An entry of another size is dropped too, since a reader
     * of that size cannot lay it. Another channel's entry is kept -- a per-channel source has one
     * per channel, and they share the row's identity, not its map. The row index hands the row's
     * own keys; the copy stands because the drop below edits the index itself. */
    if (const Vector<ShaderBakeKey> *row_keys_ptr = shader_bake_row_index().lookup_ptr(
            shader_bake_row_id(key)))
    {
      const Vector<ShaderBakeKey> row_keys = *row_keys_ptr;
      for (const ShaderBakeKey &old_key : row_keys) {
        if (old_key == key) {
          continue;
        }
        if (const std::unique_ptr<ShaderBakeEntry> *entry = shader_bake_cache().lookup_ptr(
                old_key))
        {
          if (Image *old = (*entry)->image) {
            id_us_min(&old->id);
          }
        }
        shader_bake_cache_remove(old_key);
      }
    }
    auto entry = std::make_unique<ShaderBakeEntry>();
    entry->key = key;
    entry->width = width;
    entry->height = height;
    /* A shared stale image is still the previous answer; the render that fills this identity
     * marks the entry rendered. A fresh empty one is due by construction. */
    entry->rendered = false;
    entry->image = image;
    /* Add, not overwrite: the re-check above proved the key absent under this lock, and another
     * writer cannot land in between. */
    shader_bake_cache().add(key, std::move(entry));
    shader_bake_row_index_add(key);
  }
  return image;
}

bool BKE_paint_layers_shader_bake_due(const Material &ma,
                                      const MaterialPaintLayer &row,
                                      const PaintLayerTileContext &tile,
                                      const int width,
                                      const int height)
{
  if (!shader_bake_kind_of(row) || shader_bake_hooks.render == nullptr ||
      row.kind_tree == nullptr || width <= 0 || height <= 0)
  {
    return false;
  }
  /* Every channel the row answers is asked: a per-channel source with one landed channel and one
   * empty one is due until the render fills them together. */
  std::lock_guard lock(shader_bake_cache_mutex());
  for (const int channel : shader_bake_channels(ma, row)) {
    const ShaderBakeKey key = shader_bake_key(ma, row, tile.tile_number, channel);
    const std::unique_ptr<ShaderBakeEntry> *entry = shader_bake_cache().lookup_ptr(key);
    if (entry == nullptr) {
      return true;
    }
    /* An entry of another size is the old texel space, and an entry a reader made is an empty map
     * no render landed in yet: both are due. */
    if ((*entry)->width != width || (*entry)->height != height || !(*entry)->rendered ||
        (*entry)->image == nullptr)
    {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_shader_bake_compute(Main &bmain,
                                          const Material &ma,
                                          const MaterialPaintLayer &row,
                                          const PaintLayerTileContext &tile,
                                          const int width,
                                          const int height)
{
  if (!shader_bake_kind_of(row) || shader_bake_hooks.render == nullptr ||
      row.kind_tree == nullptr || width <= 0 || height <= 0)
  {
    return false;
  }
  /* A render creates the cache's images in \a bmain: main-thread business, refused rather than
   * trusted to the debug assert (see #BKE_paint_layers_shader_bake_image_get). */
  if (!BLI_thread_is_main()) {
    return false;
  }
  /* The channels the row answers, from the kind: one on the `Result` contract, the tree's own
   * named ones on the per-channel contract, the record channels on a generator -- one render
   * lands them all, the way the bake renders every requested channel together. */
  const Vector<int> channels = shader_bake_channels(ma, row);

  Vector<Image *> images;
  Vector<ImBuf *> buffers;
  Vector<void *> locks;
  Vector<PaintLayerShaderBakeOutput> outputs;
  const bool generator_ask = row.source == MA_PAINT_LAYER_SOURCE_GENERATOR &&
                             row.kind_subtype == MA_PAINT_LAYER_GENERATOR_NODE_GROUP;
  for (const int channel : channels) {
    Image *image = BKE_paint_layers_shader_bake_image_get(
        bmain, ma, row, channel, tile, width, height);
    if (image == nullptr) {
      continue;
    }
    void *lock = nullptr;
    ImBuf *buffer = BKE_image_acquire_ibuf(image, nullptr, &lock);
    if (buffer == nullptr || buffer->float_buffer.data == nullptr || buffer->x != width ||
        buffer->y != height)
    {
      BKE_image_release_ibuf(image, buffer, lock);
      continue;
    }
    images.append(image);
    buffers.append(buffer);
    locks.append(lock);
    PaintLayerShaderBakeOutput output;
    output.channel = channel;
    /* The write the render lands in: the for-write accessor keeps the sharing right (a shared
     * buffer copies, a mutable one tags), the way every pixel write into an ImBuf takes it. */
    output.pixels = buffer->float_data_for_write();
    if (generator_ask) {
      /* The ramp context rides with the pixels: the band the channel's tail reads and the ends
       * its null-band fallback takes, filled straight from the row the way the describe fills
       * the generator payload. Every other ask carries the defaults -- their answers are raw
       * colours, and the backend wires the tree's own output through. */
      generator_shader_bake_ramp_fill(
          row, channel, output.ramp, output.scalar_start, output.scalar_end);
    }
    outputs.append(output);
  }
  if (outputs.is_empty()) {
    return false;
  }
  PaintLayerShaderBakeRequest request;
  request.tree = row.kind_tree;
  request.parameters = row.system_properties;
  request.width = width;
  request.height = height;
  request.tile_number = tile.tile_number;
  request.generator = generator_ask;
  /* The result-has-alpha ask is the filter kernel's alone: a source tree's coverage is its map's
   * own alpha already, and the generator's is its ramp tail's, so neither carries the flag and
   * the answer stays the straight map the cache has always served. */
  const bool rendered = shader_bake_hooks.render(
      request, MutableSpan<PaintLayerShaderBakeOutput>(outputs));
  for (const int i : images.index_range()) {
    if (rendered) {
      /* The acquired buffer is the one the render wrote through: the mark is what tells the
       * image's readers (and the bake hash's pixel versions) the pixels moved. */
      BKE_image_mark_dirty(images[i], buffers[i]);
    }
    /* The release the acquire paired with, whatever the render answered. */
    BKE_image_release_ibuf(images[i], buffers[i], locks[i]);
  }
  if (rendered) {
    /* The pixels now answer this identity: the due-check stops asking until the definition or
     * the values move again. Only the channels that actually got a buffer are marked -- a channel
     * whose image could not be acquired was left out of #outputs and did not render, so marking it
     * would keep its empty map from ever being asked for again. */
    std::lock_guard lock(shader_bake_cache_mutex());
    for (const PaintLayerShaderBakeOutput &output : outputs) {
      const ShaderBakeKey key = shader_bake_key(ma, row, tile.tile_number, output.channel);
      if (const std::unique_ptr<ShaderBakeEntry> *entry = shader_bake_cache().lookup_ptr(key)) {
        (*entry)->rendered = true;
      }
    }
  }
  return rendered;
}

void BKE_paint_layers_shader_bake_free_material(const uint32_t material_session_uid)
{
  std::lock_guard lock(shader_bake_cache_mutex());
  Vector<ShaderBakeKey> dropped;
  for (const auto &[key, entry] : shader_bake_cache().items()) {
    if (key.material_uid == material_session_uid) {
      dropped.append(key);
    }
  }
  for (const ShaderBakeKey &key : dropped) {
    if (const std::unique_ptr<ShaderBakeEntry> *entry = shader_bake_cache().lookup_ptr(key)) {
      if (Image *old = (*entry)->image) {
        id_us_min(&old->id);
      }
      /* The row index goes with the cache: a dropped key left tracked would point the reads at
       * entries that no longer stand. */
      shader_bake_cache_remove(key);
    }
  }
}

void BKE_paint_layers_shader_bake_free_all()
{
  /* The entries are dropped without touching the image users: the cache's images die with the
   * Main they were created in (a file close, a load, an undo), and the callers of this clear run
   * exactly where that Main's lifetime ends or has ended -- a user count decrement here would be
   * a write into freed IDs. The mid-session drops go through
   * #BKE_paint_layers_shader_bake_free_material instead, whose Main is alive and whose images are
   * orphans the purge takes. */
  std::lock_guard lock(shader_bake_cache_mutex());
  shader_bake_cache().clear();
  shader_bake_row_index().clear();
}

/** \} */

}  // namespace blender
