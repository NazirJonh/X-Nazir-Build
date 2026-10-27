/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * The Layer source bake behind a PBR Paint brush. ROW renders one stack row's isolated content on
 * a worker through the same CPU composite a row bake uses; FULL reuses the material bake on the
 * layered owner, whose generated tree already is the whole stack's output.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include "ED_brush_layer_source.hh"
#include "ED_material_bake.hh"

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_time.h"
#include "BLI_timer.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_brush_layer_source.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_report.hh"

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_windowmanager_types.h"

#include "IMB_imbuf.hh"

#include "MEM_guardedalloc.h"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed::brush_layer_source {

namespace {

constexpr int64_t brush_layer_source_cache_budget = int64_t(512) << 20;
constexpr double brush_layer_source_retry_seconds = 0.25;
constexpr double brush_layer_source_poll_seconds = 0.5;

struct LayerSourceKey {
  uint32_t owner_session_uid = 0;
  int8_t scope = 0;
  bUUID marker = {};
  int size = 0;
  uint64_t content_hash = 0;

  uint64_t hash() const
  {
    const uint64_t marker_hash = UUID(marker).hash();
    return get_default_hash(get_default_hash(owner_session_uid, int(scope), marker_hash),
                            size,
                            content_hash);
  }

  friend bool operator==(const LayerSourceKey &a, const LayerSourceKey &b)
  {
    return a.owner_session_uid == b.owner_session_uid && a.scope == b.scope &&
           BLI_uuid_equal(a.marker, b.marker) && a.size == b.size &&
           a.content_hash == b.content_hash;
  }
};

struct CacheEntry {
  std::shared_ptr<const MaterialSourceBake> bake;
  Vector<uint32_t> image_session_uids;
  bool stale = false;
  int64_t byte_size = 0;
  uint64_t last_used_serial = 0;
};

struct PendingRefresh {
  uint64_t content_hash = 0;
  double due = 0.0;
  bool force = false;
};

/** All process-wide bake state in one place; the accessors below keep call sites short. */
struct SourceBakeState {
  std::mutex mutex;
  Map<LayerSourceKey, CacheEntry> cache;
  Set<LayerSourceKey> pending_jobs;
  /** Row bakes whose maps a source needs before its own content can be rendered. */
  Set<LayerSourceKey> waiting_rows;
  Map<uint32_t, PendingRefresh> pending_refresh;
  /**
   * The last time each #Image was reported changed, by session UID. A render that started before
   * an edit must not land as current: pixels have no content hash, so this is what lets a bake job
   * compare the state it sampled against the state at commit.
   */
  Map<uint32_t, uint64_t> image_change_serials;
  uint64_t image_change_counter = 0;
  uint64_t use_serial = 0;
  /** Per brush, the source state a notification last asked a bake for. */
  Map<uint32_t, uint64_t> requested_hashes;
};

SourceBakeState &bake_state()
{
  static SourceBakeState state;
  return state;
}

std::mutex &cache_mutex()
{
  return bake_state().mutex;
}

Map<LayerSourceKey, CacheEntry> &bake_cache()
{
  return bake_state().cache;
}

Set<LayerSourceKey> &pending_jobs()
{
  return bake_state().pending_jobs;
}

Set<LayerSourceKey> &waiting_rows()
{
  return bake_state().waiting_rows;
}

Map<uint32_t, PendingRefresh> &pending_refresh()
{
  return bake_state().pending_refresh;
}

Map<uint32_t, uint64_t> &image_change_serials()
{
  return bake_state().image_change_serials;
}

uint64_t &image_change_counter()
{
  return bake_state().image_change_counter;
}

uint64_t &use_serial()
{
  return bake_state().use_serial;
}

Map<uint32_t, uint64_t> &state_requested_hashes()
{
  return bake_state().requested_hashes;
}

char refresh_timer_tag = 0;

uintptr_t refresh_timer_uuid()
{
  return reinterpret_cast<uintptr_t>(&refresh_timer_tag);
}

bool is_layer_source(const BrushMaterialPaint &brush_paint)
{
  return BKE_paint_material_source_is_layer(brush_paint) && brush_paint.source_layer_owner != nullptr;
}

bool layer_source_key_from_paint(const BrushMaterialPaint &brush_paint, LayerSourceKey &r_key)
{
  if (!is_layer_source(brush_paint) || brush_paint.source_bake_size <= 0) {
    return false;
  }
  const Material &owner = *brush_paint.source_layer_owner;
  if (!BKE_paint_layers_is_layered(owner)) {
    return false;
  }
  const eBrushMaterialPaintLayerSourceScope scope =
      eBrushMaterialPaintLayerSourceScope(brush_paint.layer_source_scope);
  const uint64_t content_hash = bke::BKE_brush_layer_source_content_hash(
      owner, scope, brush_paint.source_layer_marker);
  if (content_hash == 0) {
    return false;
  }

  r_key.owner_session_uid = owner.id.session_uid;
  r_key.scope = int8_t(scope);
  r_key.marker = brush_paint.source_layer_marker;
  r_key.size = brush_paint.source_bake_size;
  r_key.content_hash = content_hash;
  return true;
}

bool row_scope(const LayerSourceKey &key)
{
  return key.scope == BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_ROW;
}

Material *material_from_session_uid(Main &bmain, const uint32_t session_uid)
{
  for (Material &material : bmain.materials) {
    if (material.id.session_uid == session_uid) {
      return &material;
    }
  }
  return nullptr;
}

bool full_source_ready(const BrushMaterialPaint &brush_paint)
{
  const Material &owner = *brush_paint.source_layer_owner;
  if (!ed::material_bake::material_source_bake_cache_contains(owner, brush_paint.source_bake_size)) {
    return false;
  }
  return !ed::material_bake::material_source_bake_is_stale(owner, brush_paint.source_bake_size);
}

bool cache_entry_ready(const LayerSourceKey &key)
{
  std::lock_guard lock(cache_mutex());
  const CacheEntry *entry = bake_cache().lookup_ptr(key);
  return entry != nullptr && !entry->stale;
}

/** The Material rows a row's content reads, whose renders must land before it can be composited. */
void forced_rows_collect(const Material &ma,
                         const MaterialPaintLayer &row,
                         Vector<bUUID> &r_markers)
{
  if (BKE_paint_layers_material_row_render_forced_wanted(ma, row)) {
    r_markers.append(row.marker);
  }
  for (const MaterialPaintLayer &effect : row.effects) {
    if (BKE_paint_layers_material_row_render_forced_wanted(ma, effect)) {
      r_markers.append(effect.marker);
    }
  }
  for (const MaterialPaintLayer &item : row.mask_stack) {
    if (BKE_paint_layers_material_row_render_forced_wanted(ma, item)) {
      r_markers.append(item.marker);
    }
  }
  for (const MaterialPaintLayer &child : row.children) {
    forced_rows_collect(ma, child, r_markers);
  }
}

void cache_trim_to_budget(const LayerSourceKey &keep)
{
  int64_t total = 0;
  for (const CacheEntry &entry : bake_cache().values()) {
    total += entry.byte_size;
  }
  while (total > brush_layer_source_cache_budget) {
    LayerSourceKey oldest_key;
    uint64_t oldest_serial = UINT64_MAX;
    bool found = false;
    for (const auto item : bake_cache().items()) {
      if (item.key == keep) {
        continue;
      }
      if (!found || item.value.last_used_serial < oldest_serial) {
        found = true;
        oldest_serial = item.value.last_used_serial;
        oldest_key = item.key;
      }
    }
    if (!found) {
      /* The just-stored bake alone may exceed the budget; keeping it is the point. */
      break;
    }
    total -= bake_cache().lookup(oldest_key).byte_size;
    bake_cache().remove(oldest_key);
  }
}

int64_t bake_count_images(const MaterialSourceBake &bake)
{
  int64_t count = 0;
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (bake.channel_image(eMaterialPaintChannel(channel)) != nullptr) {
      count++;
    }
  }
  return count;
}

void cache_store(const LayerSourceKey &key,
                 std::shared_ptr<const MaterialSourceBake> bake,
                 Vector<uint32_t> image_session_uids)
{
  CacheEntry entry;
  entry.byte_size = int64_t(key.size) * key.size * 4 * int64_t(sizeof(float)) *
                    int64_t(bake_count_images(*bake));
  entry.last_used_serial = ++use_serial();
  entry.bake = std::move(bake);
  entry.image_session_uids = std::move(image_session_uids);

  std::lock_guard lock(cache_mutex());
  /* Superseded revisions of the same source are unreachable once this one lands; a stroke that
   * still holds one keeps it alive through its own shared_ptr. */
  bake_cache().remove_if([&](const auto item) {
    return item.key.owner_session_uid == key.owner_session_uid && item.key.scope == key.scope &&
           BLI_uuid_equal(item.key.marker, key.marker) && item.key.size == key.size &&
           !(item.key == key);
  });
  bake_cache().lookup_or_add_default(key) = std::move(entry);
  cache_trim_to_budget(key);
}

/** One row content render. Owns the localized material until the job free callback. */
struct LayerSourceJob {
  LayerSourceKey key;
  Material *owner_copy = nullptr;
  std::array<ImBuf *, PAINT_MATERIAL_CHANNEL_NUM> images{};
  MaterialSourceResolve resolve;
  Vector<uint32_t> image_session_uids;
  /** (session UID, change serial) of every source image when the render started. */
  Vector<std::pair<uint32_t, uint64_t>> image_serials;
  bool cancelled = false;

  LayerSourceJob()
  {
    resolve.channels.fill(ChannelResolution::Unavailable);
    resolve.reasons.fill(ChannelUnavailableReason::NoSocketForChannel);
    resolve.constants.fill(float4(0.0f));
    resolve.images.fill(ChannelSourceImage{});
  }
};

void job_start(void *customdata, wmJobWorkerStatus *worker_status)
{
  LayerSourceJob &job = *static_cast<LayerSourceJob *>(customdata);
  if (job.owner_copy == nullptr) {
    return;
  }
  const MaterialPaintLayer *row = BKE_paint_layers_find(*job.owner_copy, job.key.marker);
  if (row == nullptr) {
    return;
  }

  const int size = job.key.size;
  const int64_t pixel_num = int64_t(size) * size;
  int done = 0;
  /* The render has to be handed a coverage buffer, but a brush source only reads the content alpha
   * that already travels in the color. One scratch buffer serves every channel instead of a fresh
   * allocation of that size per channel. */
  Vector<float> coverage(pixel_num);
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (worker_status != nullptr && worker_status->stop) {
      job.cancelled = true;
      return;
    }
    const int channel = int(info.channel);

    /* Rendered straight into the buffer the bake keeps, so the color is not held twice. */
    ImBuf *ibuf = IMB_allocImBuf(size, size, ImBufFlags::Zero);
    if (ibuf == nullptr) {
      continue;
    }
    ibuf->channels = 4;
    if (!IMB_alloc_float_pixels(ibuf, 4, false)) {
      IMB_freeImBuf(ibuf);
      continue;
    }
    const bool rendered = BKE_paint_layers_bake_render_node(*job.owner_copy,
                                                            *row,
                                                            channel,
                                                            size,
                                                            ibuf->float_data_for_write(),
                                                            coverage.data());
    done++;
    if (worker_status != nullptr) {
      worker_status->progress = float(done) / float(PAINT_MATERIAL_CHANNEL_NUM);
      worker_status->do_update = true;
    }
    if (!rendered) {
      IMB_freeImBuf(ibuf);
      continue;
    }
    job.images[channel] = ibuf;
    job.resolve.channels[channel] = ChannelResolution::Baked;
    job.resolve.reasons[channel] = ChannelUnavailableReason::None;
  }
}

void job_end(void *customdata)
{
  LayerSourceJob &job = *static_cast<LayerSourceJob *>(customdata);
  Main *bmain = G_MAIN;
  bool committed = false;

  if (!job.cancelled && job.owner_copy != nullptr && bmain != nullptr) {
    Material *owner = material_from_session_uid(*bmain, job.key.owner_session_uid);
    bool current = owner != nullptr && BKE_paint_layers_is_layered(*owner) &&
                   BKE_paint_layers_find(*owner, job.key.marker) != nullptr;
    if (current) {
      const eBrushMaterialPaintLayerSourceScope scope =
          eBrushMaterialPaintLayerSourceScope(job.key.scope);
      current = bke::BKE_brush_layer_source_content_hash(
                    *owner, scope, job.key.marker) == job.key.content_hash;
    }
    if (current) {
      /* The pixels the worker sampled must still be the pixels the source has now. Any edit to a
       * dependency while the render ran makes this result a mix of two states. */
      std::lock_guard lock(cache_mutex());
      for (const auto &source : job.image_serials) {
        if (image_change_serials().lookup_default(source.first, 0) != source.second) {
          current = false;
          break;
        }
      }
    }
    if (current) {
      auto bake = std::make_shared<const MaterialSourceBake>(job.resolve, job.images);
      /* The snapshot owns the buffers now. */
      job.images.fill(nullptr);
      cache_store(job.key, std::move(bake), job.image_session_uids);
      committed = true;
    }
  }

  {
    std::lock_guard lock(cache_mutex());
    pending_jobs().remove(job.key);
    if (!committed) {
      waiting_rows().remove(job.key);
    }
  }
  if (committed) {
    WM_main_add_notifier(NC_BRUSH | NA_EDITED, nullptr);
  }
}

void job_free(void *customdata)
{
  LayerSourceJob *job = static_cast<LayerSourceJob *>(customdata);
  for (ImBuf *ibuf : job->images) {
    if (ibuf != nullptr) {
      IMB_freeImBuf(ibuf);
    }
  }
  if (job->owner_copy != nullptr) {
    BKE_id_free(nullptr, &job->owner_copy->id);
  }
  {
    std::lock_guard lock(cache_mutex());
    pending_jobs().remove(job->key);
  }
  MEM_delete(job);
}

enum class EnsureResult {
  /** Nothing left to retry for this request. */
  Done,
  /** The source still needs something else to land; the refresh timer retries. */
  Retry,
};

EnsureResult ensure_row_bake(Main &bmain,
                             const void *job_owner,
                             const BrushMaterialPaint &brush_paint,
                             const LayerSourceKey &key)
{
  {
    std::lock_guard lock(cache_mutex());
    const CacheEntry *entry = bake_cache().lookup_ptr(key);
    if (entry != nullptr && !entry->stale) {
      return EnsureResult::Done;
    }
    if (pending_jobs().contains(key)) {
      return EnsureResult::Done;
    }
  }

  Material &owner = *brush_paint.source_layer_owner;
  const MaterialPaintLayer *row = BKE_paint_layers_find(owner, key.marker);
  if (row == nullptr) {
    return EnsureResult::Done;
  }

  Vector<bUUID> forced;
  forced_rows_collect(owner, *row, forced);
  if (!forced.is_empty()) {
    material_bake::material_bake_layered_rows_ensure(bmain, owner, false, forced);
    std::lock_guard lock(cache_mutex());
    waiting_rows().add(key);
    return EnsureResult::Retry;
  }
  {
    std::lock_guard lock(cache_mutex());
    waiting_rows().remove(key);
  }

  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain.wm.first);
  if (wm == nullptr) {
    return EnsureResult::Done;
  }

  LayerSourceJob *job = MEM_new<LayerSourceJob>(__func__);
  job->key = key;
  job->owner_copy = id_cast<Material *>(BKE_id_copy_ex(
      nullptr,
      &owner.id,
      nullptr,
      LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE | LIB_ID_COPY_NO_ANIMDATA));
  if (job->owner_copy == nullptr) {
    MEM_delete(job);
    return EnsureResult::Done;
  }

  Vector<Image *> images;
  bke::BKE_brush_layer_source_images_collect(
      owner, eBrushMaterialPaintLayerSourceScope(key.scope), key.marker, images);
  job->image_session_uids.reserve(images.size());
  job->image_serials.reserve(images.size());
  {
    std::lock_guard lock(cache_mutex());
    for (const Image *image : images) {
      const uint32_t session_uid = image->id.session_uid;
      if (job->image_session_uids.contains(session_uid)) {
        continue;
      }
      job->image_session_uids.append(session_uid);
      job->image_serials.append(
          {session_uid, image_change_serials().lookup_default(session_uid, 0)});
    }
    pending_jobs().add(key);
  }

  wmJob *wm_job = WM_jobs_get(wm,
                              static_cast<wmWindow *>(wm->windows.first),
                              job_owner,
                              "Baking brush layer source...",
                              WM_JOB_EXCL_RENDER | WM_JOB_PROGRESS,
                              WM_JOB_TYPE_BRUSH_LAYER_SOURCE);
  WM_jobs_customdata_set(wm_job, job, job_free);
  WM_jobs_timer(wm_job, 0.2, NC_BRUSH, NC_BRUSH);
  WM_jobs_callbacks(wm_job, job_start, nullptr, nullptr, job_end);
  WM_jobs_start(wm, wm_job);
  return EnsureResult::Done;
}

/** Shared by the brush path and the Shape path: \a job_owner only identifies the WM job, so a
 * caller with no #Brush (a Shape part) can pass a stable pointer to its own texture block. */
EnsureResult ensure_impl(Main &bmain,
                         const BrushMaterialPaint &brush_paint,
                         const void *job_owner)
{
  if (!is_layer_source(brush_paint)) {
    return EnsureResult::Done;
  }

  LayerSourceKey key;
  if (!layer_source_key_from_paint(brush_paint, key)) {
    return EnsureResult::Done;
  }

  if (brush_paint.layer_source_scope == BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_FULL) {
    Material &owner = *brush_paint.source_layer_owner;
    if (full_source_ready(brush_paint)) {
      return EnsureResult::Done;
    }
    /* The owner's rows landing rewrites its generated tree, so a bake that finishes can already be
     * for an older state. Keep asking until the exact one is current. */
    if (material_bake::material_source_bake_is_pending(owner, brush_paint.source_bake_size)) {
      return EnsureResult::Retry;
    }
    /* The layered owner's generated tree is the complete stack's output, so the material bake
     * already is the Full Material source. Rows left live by a pending bake are values the graph
     * evaluates directly, not a wrong result. */
    material_bake::material_bake_layered_rows_ensure(bmain, owner);
    material_bake::material_source_bake_ensure(bmain, owner, brush_paint.source_bake_size);
    return EnsureResult::Retry;
  }

  return ensure_row_bake(bmain, job_owner, brush_paint, key);
}

EnsureResult ensure_impl(Main &bmain, const Brush &brush, const bool force)
{
  const BrushMaterialPaint *brush_paint = brush.material_paint;
  if (brush_paint == nullptr) {
    return EnsureResult::Done;
  }
  if (brush_paint->source_refresh_mode == BRUSH_MATERIAL_PAINT_SOURCE_REFRESH_MANUAL && !force) {
    return EnsureResult::Done;
  }

  return ensure_impl(bmain, *brush_paint, &brush);
}

Brush *brush_from_session_uid(Main &bmain, const uint32_t session_uid)
{
  for (Brush &brush : bmain.brushes) {
    if (brush.id.session_uid == session_uid) {
      return &brush;
    }
  }
  return nullptr;
}

void arm_refresh(const Brush &brush,
                 const LayerSourceKey &key,
                 const bool force,
                 const double delay_seconds)
{
  {
    std::lock_guard lock(cache_mutex());
    PendingRefresh *existing = pending_refresh().lookup_ptr(brush.id.session_uid);
    if (!force && existing != nullptr && existing->content_hash == key.content_hash) {
      /* Already waiting for this exact source state; do not push the deadline out. */
      return;
    }
    PendingRefresh &entry = pending_refresh().lookup_or_add_default(brush.id.session_uid);
    entry.content_hash = key.content_hash;
    entry.due = BLI_time_now_seconds() + delay_seconds;
    entry.force = force;
  }

  if (BLI_timer_is_registered(refresh_timer_uuid())) {
    return;
  }

  BLI_timer_register(
      refresh_timer_uuid(),
      [](uintptr_t /*uuid*/, void * /*user_data*/) -> double {
        Main *bmain = G_MAIN;
        if (bmain == nullptr || bmain->wm.first == nullptr) {
          std::lock_guard lock(cache_mutex());
          pending_refresh().clear();
          return -1.0;
        }

        const double now = BLI_time_now_seconds();
        Vector<uint32_t> due;
        bool any_future = false;
        {
          std::lock_guard lock(cache_mutex());
          for (const auto item : pending_refresh().items()) {
            if (item.value.due <= now) {
              due.append(item.key);
            }
            else {
              any_future = true;
            }
          }
        }

        for (const uint32_t brush_uid : due) {
          PendingRefresh record;
          {
            std::lock_guard lock(cache_mutex());
            const PendingRefresh *stored = pending_refresh().lookup_ptr(brush_uid);
            if (stored == nullptr) {
              continue;
            }
            record = *stored;
          }

          Brush *brush = brush_from_session_uid(*bmain, brush_uid);
          if (brush == nullptr || brush->material_paint == nullptr) {
            std::lock_guard lock(cache_mutex());
            pending_refresh().remove(brush_uid);
            continue;
          }

          LayerSourceKey key;
          if (!layer_source_key_from_paint(*brush->material_paint, key)) {
            std::lock_guard lock(cache_mutex());
            pending_refresh().remove(brush_uid);
            continue;
          }

          if (key.content_hash != record.content_hash) {
            /* The source changed again during the debounce: start the wait over. */
            std::lock_guard lock(cache_mutex());
            PendingRefresh *stored = pending_refresh().lookup_ptr(brush_uid);
            if (stored != nullptr) {
              stored->content_hash = key.content_hash;
              stored->due = now + std::max(
                                     0.0, double(brush->material_paint->source_refresh_delay));
            }
            any_future = true;
            continue;
          }

          if (ensure_impl(*bmain, *brush, record.force) == EnsureResult::Done) {
            std::lock_guard lock(cache_mutex());
            pending_refresh().remove(brush_uid);
          }
          else {
            std::lock_guard lock(cache_mutex());
            PendingRefresh *stored = pending_refresh().lookup_ptr(brush_uid);
            if (stored != nullptr) {
              stored->due = now + brush_layer_source_retry_seconds;
            }
            any_future = true;
          }
        }
        return any_future ? 0.1 : -1.0;
      },
      nullptr,
      nullptr,
      0.1,
      false);
}


char source_poll_timer_tag = 0;

uintptr_t source_poll_timer_uuid()
{
  return reinterpret_cast<uintptr_t>(&source_poll_timer_tag);
}

/**
 * Edits reach a Layer source through paths no notifier covers: the owner's generated tree is
 * rebuilt by its own debounce, rows land from jobs, a map is painted while another tool is active.
 * Waiting for an event therefore left the source stale. Instead, while any brush has a Layer
 * source, the cache is checked against the source at a low rate and a stale one is requested again
 * through the usual debounce.
 */
void source_poll_arm()
{
  if (BLI_timer_is_registered(source_poll_timer_uuid())) {
    return;
  }
  BLI_timer_register(
      source_poll_timer_uuid(),
      [](uintptr_t /*uuid*/, void * /*user_data*/) -> double {
        Main *bmain = G_MAIN;
        if (bmain == nullptr || bmain->wm.first == nullptr) {
          return -1.0;
        }
        bool any_source = false;
        for (Brush &brush : bmain->brushes) {
          const BrushMaterialPaint *brush_paint = brush.material_paint;
          if (brush_paint == nullptr || !is_layer_source(*brush_paint)) {
            continue;
          }
          any_source = true;
          if (brush_paint->source_refresh_mode == BRUSH_MATERIAL_PAINT_SOURCE_REFRESH_MANUAL ||
              brush_layer_source_bake_ready(*brush_paint))
          {
            continue;
          }
          LayerSourceKey key;
          if (!layer_source_key_from_paint(*brush_paint, key)) {
            continue;
          }
          bool in_flight;
          {
            std::lock_guard lock(cache_mutex());
            in_flight = pending_jobs().contains(key) || waiting_rows().contains(key) ||
                        pending_refresh().contains(brush.id.session_uid);
          }
          if (!in_flight && !row_scope(key)) {
            in_flight = material_bake::material_source_bake_is_pending(
                *brush_paint->source_layer_owner, brush_paint->source_bake_size);
          }
          if (!in_flight) {
            brush_layer_source_bake_ensure(*bmain, brush, false);
          }
        }
        return any_source ? brush_layer_source_poll_seconds : -1.0;
      },
      nullptr,
      nullptr,
      brush_layer_source_poll_seconds,
      false);
}
}  // namespace

std::shared_ptr<const MaterialSourceBake> brush_layer_source_bake_get(
    const BrushMaterialPaint &brush_paint)
{
  if (!is_layer_source(brush_paint)) {
    return nullptr;
  }
  if (brush_paint.layer_source_scope == BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_FULL) {
    /* Like the Material source: an older bake of the owner is served while the exact one renders,
     * so the cursor preview and a running stroke keep their texture. #brush_layer_source_bake_ready
     * is what tells a current bake from that. */
    return material_bake::material_source_bake_get(*brush_paint.source_layer_owner,
                                                   brush_paint.source_bake_size);
  }

  LayerSourceKey key;
  if (!layer_source_key_from_paint(brush_paint, key)) {
    return nullptr;
  }
  std::lock_guard lock(cache_mutex());
  CacheEntry *entry = bake_cache().lookup_ptr(key);
  if (entry == nullptr || entry->stale) {
    return nullptr;
  }
  entry->last_used_serial = ++use_serial();
  return entry->bake;
}

bool brush_layer_source_bake_ready(const BrushMaterialPaint &brush_paint)
{
  if (is_layer_source(brush_paint) &&
      brush_paint.layer_source_scope == BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_FULL)
  {
    return full_source_ready(brush_paint);
  }
  return brush_layer_source_bake_get(brush_paint) != nullptr;
}

bool brush_layer_source_is_stale(const BrushMaterialPaint &brush_paint)
{
  if (!is_layer_source(brush_paint)) {
    return false;
  }
  if (brush_paint.layer_source_scope == BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_FULL) {
    return !full_source_ready(brush_paint);
  }
  LayerSourceKey key;
  if (!layer_source_key_from_paint(brush_paint, key)) {
    return true;
  }
  return !cache_entry_ready(key);
}

bool brush_layer_source_is_baking(const Brush &brush)
{
  const BrushMaterialPaint *brush_paint = brush.material_paint;
  if (brush_paint == nullptr || !is_layer_source(*brush_paint)) {
    return false;
  }

  if (brush_paint->layer_source_scope == BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_FULL) {
    return material_bake::material_source_bake_is_pending(*brush_paint->source_layer_owner,
                                                          brush_paint->source_bake_size);
  }

  LayerSourceKey key;
  if (!layer_source_key_from_paint(*brush_paint, key)) {
    return false;
  }
  std::lock_guard lock(cache_mutex());
  return pending_jobs().contains(key) || waiting_rows().contains(key) ||
         pending_refresh().contains(brush.id.session_uid);
}

void brush_layer_source_bake_ensure(Main &bmain, const Brush &brush, const bool force)
{
  const BrushMaterialPaint *brush_paint = brush.material_paint;
  if (brush_paint == nullptr || !is_layer_source(*brush_paint)) {
    return;
  }
  source_poll_arm();
  if (brush_paint->source_refresh_mode == BRUSH_MATERIAL_PAINT_SOURCE_REFRESH_MANUAL && !force) {
    return;
  }

  if (brush_layer_source_bake_ready(*brush_paint)) {
    std::lock_guard lock(cache_mutex());
    pending_refresh().remove(brush.id.session_uid);
    return;
  }

  LayerSourceKey key;
  if (!layer_source_key_from_paint(*brush_paint, key)) {
    return;
  }

  const double delay = force ? 0.0 :
                             std::clamp(double(brush_paint->source_refresh_delay), 0.0, 60.0);
  if (delay > 0.0) {
    arm_refresh(brush, key, force, delay);
    return;
  }

  if (ensure_impl(bmain, brush, force) == EnsureResult::Retry) {
    arm_refresh(brush, key, force, brush_layer_source_retry_seconds);
  }
  else {
    std::lock_guard lock(cache_mutex());
    pending_refresh().remove(brush.id.session_uid);
  }
}

void brush_layer_source_bake_ensure(const bContext &C, const Brush &brush)
{
  Main *bmain = CTX_data_main(&C);
  if (bmain != nullptr) {
    brush_layer_source_bake_ensure(*bmain, brush, false);
  }
}

void brush_layer_source_bake_ensure(Main &bmain,
                                    const BrushMaterialPaint &brush_paint,
                                    const void *job_owner)
{
  /* Redraw-frequency entry for sources with no #Brush (a Shape part's Layer material). It skips
   * the brush's Manual/debounce refresh modes and arms no refresh timer: the caller resolves again
   * on the next redraw, and the job's own #NC_BRUSH timer is what makes that redraw happen. */
  ensure_impl(bmain, brush_paint, job_owner);
}

bool brush_layer_source_stroke_ready(Main &bmain, const Brush &brush, ReportList *reports)
{
  const BrushMaterialPaint *brush_paint = brush.material_paint;
  if (brush_paint == nullptr || !BKE_paint_material_source_is_layer(*brush_paint) ||
      brush_layer_source_bake_ready(*brush_paint))
  {
    return true;
  }
  brush_layer_source_bake_ensure(bmain, brush, false);
  BKE_report(reports,
             RPT_INFO,
             "Brush Layer source is not ready; wait for the bake or press Update Now");
  return false;
}

void brush_layer_source_bake_update_now(Main &bmain, const Brush &brush)
{
  {
    std::lock_guard lock(cache_mutex());
    pending_refresh().remove(brush.id.session_uid);
  }
  brush_layer_source_bake_ensure(bmain, brush, true);
}

void brush_layer_source_bake_invalidate(const Material *owner)
{
  const uint32_t owner_uid = owner != nullptr ? owner->id.session_uid : 0;
  {
    std::lock_guard lock(cache_mutex());
    if (owner == nullptr) {
      bake_cache().clear();
      pending_jobs().clear();
      waiting_rows().clear();
      pending_refresh().clear();
      image_change_serials().clear();
      state_requested_hashes().clear();
      BLI_timer_unregister(source_poll_timer_uuid());
      BLI_timer_unregister(refresh_timer_uuid());
    }
    else {
      bake_cache().remove_if([&](const auto item) {
        return item.key.owner_session_uid == owner_uid;
      });
      pending_jobs().remove_if([&](const LayerSourceKey &key) {
        return key.owner_session_uid == owner_uid;
      });
      waiting_rows().remove_if([&](const LayerSourceKey &key) {
        return key.owner_session_uid == owner_uid;
      });
    }
  }
}

void brush_layer_source_tag_image_changed(const Image &image)
{
  const uint32_t session_uid = image.id.session_uid;
  std::lock_guard lock(cache_mutex());
  image_change_serials().add_overwrite(session_uid, ++image_change_counter());
  for (auto item : bake_cache().items()) {
    if (item.value.image_session_uids.contains(session_uid)) {
      item.value.stale = true;
    }
  }
}

void brush_layer_source_notify_changed(Main &bmain, const bool pixels_changed)
{
  for (Brush &brush : bmain.brushes) {
    const BrushMaterialPaint *brush_paint = brush.material_paint;
    if (brush_paint == nullptr || !is_layer_source(*brush_paint) ||
        brush_layer_source_bake_ready(*brush_paint))
    {
      continue;
    }
    LayerSourceKey key;
    if (!layer_source_key_from_paint(*brush_paint, key)) {
      continue;
    }
    {
      std::lock_guard lock(cache_mutex());
      if (!pixels_changed) {
        /* The bake itself edits the owner (row maps land, the generated tree is rebuilt), which
         * arrives here as a material update. Answering it would bake forever, so the state a bake is
         * already running or waiting for is left alone; any other state is a real edit. */
        const uint64_t *requested = state_requested_hashes().lookup_ptr(brush.id.session_uid);
        /* A state that was requested but has nothing running or waiting any more was discarded
         * (an edit landed mid-render), and must be asked for again. */
        const bool in_flight = pending_jobs().contains(key) || waiting_rows().contains(key) ||
                               pending_refresh().contains(brush.id.session_uid);
        if (requested != nullptr && *requested == key.content_hash && in_flight) {
          continue;
        }
      }
      state_requested_hashes().add_overwrite(brush.id.session_uid, key.content_hash);
      /* Stale: the deadline moves out with every edit instead of firing mid-burst. */
      pending_refresh().remove(brush.id.session_uid);
    }
    brush_layer_source_bake_ensure(bmain, brush, false);
  }
}

bool brush_layer_source_preview_get(
    const BrushMaterialPaint &brush_paint,
    const PaintModeSettings &mode_settings,
    const int visible_material_channels,
    MaterialSourcePreview &r_preview,
    std::shared_ptr<const MaterialSourceBake> &r_bake,
    const std::optional<eMaterialPaintChannel> only_channel)
{
  r_preview.ibuf = nullptr;
  r_preview.constant = float4(0.0f);
  r_preview.usable = false;
  r_preview.mtex = dna::shallow_copy(MTex());

  r_bake = brush_layer_source_bake_get(brush_paint);
  if (r_bake == nullptr) {
    return false;
  }

  for (const eMaterialPaintChannel channel : BKE_paint_material_channel_preview_order()) {
    if (only_channel) {
      if (channel != *only_channel) {
        continue;
      }
    }
    else if (!BKE_paint_material_channel_is_enabled(
                 brush_paint, mode_settings, visible_material_channels, channel))
    {
      continue;
    }
    switch (r_bake->resolution(channel)) {
      case ChannelResolution::Baked: {
        const ImBuf *ibuf = r_bake->channel_image(channel);
        if (ibuf == nullptr || ibuf->float_data() == nullptr || ibuf->channels != 4) {
          continue;
        }
        r_preview.ibuf = ibuf;
        break;
      }
      case ChannelResolution::Constant:
        r_preview.constant = r_bake->channel_constant(channel);
        break;
      default:
        continue;
    }
    r_preview.usable = true;
    BKE_paint_material_channel_effective_mtex(
        brush_paint, brush_paint.channels[channel], r_preview.mtex);
    r_preview.mtex.tex = nullptr;
    return true;
  }

  r_bake.reset();
  return false;
}

}  // namespace blender::ed::brush_layer_source
