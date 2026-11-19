/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * What a Material row lays in each channel, decided per channel from what its source supplies
 * there
 * (#BKE_paint_material_source_resolve): the source's plain value and its plain Image Texture are
 * shown live, and only what is neither is rendered into a map (see `paint_layers_bake.cc`).
 *
 * The generator and the CPU composite ask the functions here and nothing else, so a row cannot
 * take part in a channel on one side and not on the other. They read the row and its source's node
 * trees and nothing of the row's maps beyond the row (the structural
 * #paint_layer_row_material_shown).
 */

#include <memory>

#include "BLI_index_range.hh"
#include "BLI_math_vector.h"
#include "BLI_ustring.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_resolve.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

/**
 * Whether \a tree itself reads the object's active UVs: a Texture Coordinate whose UV output is
 * used, a UV Map node with no layer named, or an Image Texture whose Vector input is open (it then
 * samples the active UV). A named UV Map node is the source's own layer and is not what the row
 * mapping moves; a texture driven by Generated/Object coordinates is not either. Groups are not
 * entered: the caller walks the levels the source group copies.
 */
static bool source_level_reads_uv(bNodeTree &tree)
{
  return bke::paint_layers::uv_readers_find(tree).any();
}

bool paint_layer_source_reads_uv_compute(const Material &source)
{
  if (source.nodetree == nullptr) {
    return false;
  }
  /* The levels the source group copies and maps: the root and the groups on the path to the
   * Principled BSDF. A group beside the path is shared as is, so a UV read inside it never moves. */
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  Vector<const bNode *> group_path;
  const bNode *principled = BKE_paint_material_principled_find(source, reason, &group_path);
  if (principled == nullptr || int(group_path.size()) > SOURCE_GROUP_MAX_DEPTH) {
    return false;
  }
  if (source_level_reads_uv(*source.nodetree)) {
    return true;
  }
  for (const bNode *instance : group_path) {
    if (instance->id == nullptr || GS(instance->id->name) != ID_NT) {
      continue;
    }
    bNodeTree &level = *id_cast<bNodeTree *>(instance->id);
    if (source_level_reads_uv(level)) {
      return true;
    }
  }
  return false;
}

/**
 * Whether the Image Texture \a source can be shown without the graph around it: the map is a file,
 * a generated image or a UDIM set (the composite reads the tile of the call, so a tiled map is
 * sampled per canvas tile the way a painted one is; not a sequence or a movie, whose frame is the
 * node's business), the projection is flat with nothing linked to its Vector input (so it reads the
 * UV the layers read), and the channel takes the output the node is read at (the Alpha output for a
 * coverage, the Color output for a colour).
 */
static bool live_image_usable(const ChannelSourceImage &source, const bool alpha_output)
{
  if (source.image == nullptr || source.node == nullptr || source.alpha_output != alpha_output) {
    return false;
  }
  if (!ELEM(source.image->source, IMA_SRC_FILE, IMA_SRC_GENERATED, IMA_SRC_TILED)) {
    return false;
  }
  const NodeTexImage *storage = static_cast<const NodeTexImage *>(source.node->storage);
  if (storage == nullptr || storage->projection != SHD_PROJ_FLAT) {
    return false;
  }
  const bNodeSocket *vector = bke::node_find_socket(
      const_cast<bNode &>(*source.node), SOCK_IN, "Vector"_ustr);
  /* The owner tree keeps a current cache: the node comes from a resolve, which ensures the cache of
   * every tree it walks, and only a tree update moves it since. */
  return vector == nullptr || !vector->is_directly_linked();
}

/**
 * Whether \a channel is left out of the material's set for \a row. A mask item reads the source's
 * Base Color as its grey, which is no channel of the stack, so the set does not decide it.
 */
static bool material_channel_filtered(const Material &ma,
                                      const MaterialPaintLayer &row,
                                      const int channel)
{
  return row.role != MA_PAINT_LAYER_ROLE_MASK_ITEM && paint_layer_channel_filtered(ma, channel);
}

/**
 * Whether the row's graph is shown in \a use: only the generated graph can evaluate the source's
 * shader nodes, and the Always mode asks for the render alone.
 */
static bool source_group_allowed(const MaterialPaintLayer &row, const PaintLayerMaterialUse use)
{
  return use == PaintLayerMaterialUse::Graph &&
         BKE_paint_layers_bake_mode_get(row) != MA_PAINT_LAYER_BAKE_ALWAYS;
}

bool paint_layer_material_source_group_channel(const Material &ma,
                                               const MaterialPaintLayer &row,
                                               const int channel)
{
  /* A canvas row has no live group: its maps are the render it was made from, whatever the source's
   * graph could wrap (see #BKE_paint_layers_material_row_is_canvas). */
  if (BKE_paint_layers_material_row_is_canvas(row)) {
    return false;
  }
  if (!paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE) || row.material == nullptr ||
      row.material->nodetree == nullptr || channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      (channel != PAINT_MATERIAL_CHANNEL_ALPHA && material_channel_filtered(ma, row, channel)) ||
      !BKE_paint_layers_source_group_wrappable(*row.material))
  {
    return false;
  }
  /* A normal not made by a Normal Map is not a property of the surface when evaluated live, so the
   * row shows the map rendered from it. */
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL &&
      !bke::paint_layers::source_group_normal_live(*row.material))
  {
    return false;
  }
  const std::shared_ptr<const MaterialSourceResolve> resolve = BKE_paint_layers_source_resolve(
      *row.material);
  switch (resolve->channels[channel]) {
    case ChannelResolution::Baked:
      return true;
    case ChannelResolution::Image:
      return !live_image_usable(resolve->images[channel], channel == PAINT_MATERIAL_CHANNEL_ALPHA);
    default:
      return false;
  }
}

PaintLayerMaterialSource paint_layer_material_channel_source(const Material &ma,
                                                             const MaterialPaintLayer &row,
                                                             const int channel,
                                                             const PaintLayerMaterialUse use)
{
  PaintLayerMaterialSource out;
  if (!paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE) || row.material == nullptr ||
      row.material->nodetree == nullptr || channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      channel == PAINT_MATERIAL_CHANNEL_ALPHA || material_channel_filtered(ma, row, channel))
  {
    return out;
  }
  /* A canvas row resolves to its maps alone: the render it was made from is all it lays, before
   * and after the first one (see #BKE_paint_layers_material_bake_render_row). */
  if (BKE_paint_layers_material_row_is_canvas(row)) {
    Image *baked = nullptr;
    if (paint_layer_row_material_map(ma, row, channel, &baked)) {
      out.kind = PaintLayerMaterialSourceKind::Baked;
      out.image = baked;
    }
    return out;
  }
  const std::shared_ptr<const MaterialSourceResolve> resolve = BKE_paint_layers_source_resolve(
      *row.material);
  switch (resolve->channels[channel]) {
    case ChannelResolution::Constant:
      out.kind = PaintLayerMaterialSourceKind::LiveConstant;
      copy_v3_v3(out.constant, resolve->constants[channel]);
      out.constant[3] = 1.0f;
      return out;
    case ChannelResolution::Image:
      if (live_image_usable(resolve->images[channel], false)) {
        out.kind = PaintLayerMaterialSourceKind::LiveImage;
        out.image = resolve->images[channel].image;
        out.iuser = resolve->images[channel].iuser;
        out.node = resolve->images[channel].node;
        return out;
      }
      break;
    case ChannelResolution::Unavailable:
      return out;
    default:
      break;
  }
  if (source_group_allowed(row, use) &&
      paint_layer_material_source_group_channel(ma, row, channel))
  {
    out.kind = PaintLayerMaterialSourceKind::SourceGroup;
    return out;
  }
  Image *baked = nullptr;
  if (paint_layer_row_material_map(ma, row, channel, &baked)) {
    out.kind = PaintLayerMaterialSourceKind::Baked;
    out.image = baked;
  }
  return out;
}

PaintLayerMaterialCoverage paint_layer_material_coverage_source(const Material &ma,
                                                                const MaterialPaintLayer &row,
                                                                const PaintLayerMaterialUse use)
{
  PaintLayerMaterialCoverage out;
  if (!paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE) || row.material == nullptr ||
      row.material->nodetree == nullptr)
  {
    return out;
  }
  /* A canvas row's coverage is its own maps': the render hands its content coverage back, and
   * nothing of the source's transparency is resolved here. */
  if (BKE_paint_layers_material_row_is_canvas(row)) {
    if (paint_layer_row_material_shown(ma, row) && row.bake->coverage != nullptr) {
      out.kind = PaintLayerMaterialCoverageKind::Baked;
      out.image = row.bake->coverage;
    }
    else {
      out.kind = PaintLayerMaterialCoverageKind::Pending;
    }
    return out;
  }
  const std::shared_ptr<const MaterialSourceResolve> resolve = BKE_paint_layers_source_resolve(
      *row.material);
  constexpr int alpha = PAINT_MATERIAL_CHANNEL_ALPHA;
  switch (resolve->channels[alpha]) {
    case ChannelResolution::Unavailable:
      return out;
    case ChannelResolution::Constant:
      out.kind = PaintLayerMaterialCoverageKind::LiveConstant;
      out.constant = resolve->constants[alpha].x;
      return out;
    case ChannelResolution::Image:
      if (live_image_usable(resolve->images[alpha], true)) {
        out.kind = PaintLayerMaterialCoverageKind::LiveAlpha;
        out.image = resolve->images[alpha].image;
        out.iuser = resolve->images[alpha].iuser;
        out.node = resolve->images[alpha].node;
        return out;
      }
      break;
    default:
      break;
  }
  if (source_group_allowed(row, use) && paint_layer_material_source_group_channel(ma, row, alpha))
  {
    out.kind = PaintLayerMaterialCoverageKind::SourceGroup;
    return out;
  }
  if (paint_layer_row_material_shown(ma, row) && row.bake->coverage != nullptr) {
    out.kind = PaintLayerMaterialCoverageKind::Baked;
    out.image = row.bake->coverage;
  }
  else {
    out.kind = PaintLayerMaterialCoverageKind::Pending;
  }
  return out;
}

Image *paint_layer_material_canvas_image(const Material &ma,
                                         const MaterialPaintLayer &row,
                                         const int channel)
{
  /* The role decides which map is the canvas: a mask item keeps its grey in the one channel its
   * bake was rendered into ("effect item reads the grey its bake holds"; the Base Color channel
   * unless the item was given another), a row and a correction read the channel they were asked
   * for. Everything else -- the shown test, the bake's ownership -- is the map test every Material
   * row shares. */
  const int canvas_channel = (row.role == MA_PAINT_LAYER_ROLE_MASK_ITEM) ?
                                 paint_layer_mask_source_channel(row) :
                                 channel;
  Image *image = nullptr;
  if (paint_layer_row_material_map(ma, row, canvas_channel, &image)) {
    return image;
  }
  return nullptr;
}

bool paint_layer_row_reads_source(const Material &ma,
                                  const MaterialPaintLayer &row,
                                  const int channel)
{
  if (!paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE)) {
    return false;
  }
  return paint_layer_material_channel_source(ma, row, channel).kind !=
             PaintLayerMaterialSourceKind::None &&
         paint_layer_material_coverage_source(ma, row).kind !=
             PaintLayerMaterialCoverageKind::Pending;
}

bool paint_layer_correction_reads_source(const Material &ma,
                                         const MaterialPaintLayer &effect,
                                         const int channel)
{
  if (!paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_READS_SOURCE) ||
      effect.role != MA_PAINT_LAYER_ROLE_EFFECT || paint_layer_channel_filtered(ma, channel))
  {
    return false;
  }
  const PaintLayerMaterialSource source = paint_layer_material_channel_source(ma, effect, channel);
  if (source.kind == PaintLayerMaterialSourceKind::None ||
      (channel == PAINT_MATERIAL_CHANNEL_NORMAL &&
       source.kind == PaintLayerMaterialSourceKind::LiveConstant))
  {
    return false;
  }
  const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(ma, effect);
  if (ELEM(coverage.kind,
           PaintLayerMaterialCoverageKind::Full,
           PaintLayerMaterialCoverageKind::LiveConstant))
  {
    return true;
  }
  /* A canvas correction's coverage is the map its render made, not the source's transparency
   * resolved live: it lays where its bake holds a coverage, reading like a Fill correction's own
   * map does (see #paint_layer_material_coverage_source). */
  return coverage.kind == PaintLayerMaterialCoverageKind::Baked &&
         BKE_paint_layers_material_row_is_canvas(effect);
}

int paint_layer_mask_source_channel(const MaterialPaintLayer &item)
{
  const uint32_t bits = item.mask_source_channels;
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if ((bits & (1u << channel)) != 0 && BKE_paint_layers_mask_source_channel_allowed(channel)) {
      return channel;
    }
  }
  return PAINT_MATERIAL_CHANNEL_BASE_COLOR;
}

bool paint_layer_mask_reads_source(const Material &ma, const MaterialPaintLayer &item)
{
  if (!paint_layer_kind_has_flag(item, PAINT_LAYER_KIND_READS_SOURCE) ||
      item.role != MA_PAINT_LAYER_ROLE_MASK_ITEM)
  {
    return false;
  }
  if (paint_layer_material_channel_source(ma, item, paint_layer_mask_source_channel(item)).kind ==
      PaintLayerMaterialSourceKind::None)
  {
    return false;
  }
  const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(ma, item);
  if (ELEM(coverage.kind,
           PaintLayerMaterialCoverageKind::Full,
           PaintLayerMaterialCoverageKind::LiveConstant))
  {
    return true;
  }
  /* A canvas mask item reads the grey its bake holds, with the coverage the same bake made: it
   * lays where its bake holds both (see #paint_layer_material_coverage_source). */
  return coverage.kind == PaintLayerMaterialCoverageKind::Baked &&
         BKE_paint_layers_material_row_is_canvas(item);
}

bool BKE_paint_layers_material_channel_needs_render(const Material &ma,
                                                    const MaterialPaintLayer &row,
                                                    const int channel)
{
  if (!paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE) || row.material == nullptr ||
      row.material->nodetree == nullptr || channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      (channel != PAINT_MATERIAL_CHANNEL_ALPHA && material_channel_filtered(ma, row, channel)))
  {
    return false;
  }
  /* A mask correction reads the one channel it was given, as its grey, and the source's alpha. */
  if (row.role == MA_PAINT_LAYER_ROLE_MASK_ITEM &&
      channel != paint_layer_mask_source_channel(row) && channel != PAINT_MATERIAL_CHANNEL_ALPHA)
  {
    return false;
  }
  /* A canvas row renders every channel it takes part in: its content is rendered whatever the
   * source's graph resolves there, and its coverage comes along from the same render rather than
   * an Alpha map of its own. Where that render runs decides how the alpha arrives: a narrowed
   * row's synchronous render folds the coverage out of the channels it renders
   * (#BKE_paint_layers_material_bake_render_row), so the alpha needs no map of its own; a FULL row
   * renders through the source-graph job, whose alpha target is the row's coverage map. */
  if (BKE_paint_layers_material_row_is_canvas(row)) {
    return BKE_paint_layers_material_row_narrowed(row) ?
               channel != PAINT_MATERIAL_CHANNEL_ALPHA :
               true;
  }
  const std::shared_ptr<const MaterialSourceResolve> resolve = BKE_paint_layers_source_resolve(
      *row.material);
  /* A correction lays nothing where the source's transparency is a map or a graph (see
   * #paint_layer_correction_reads_source), so there is nothing of it to render at all. */
  if (row.role != MA_PAINT_LAYER_ROLE_LAYER &&
      !ELEM(resolve->channels[PAINT_MATERIAL_CHANNEL_ALPHA],
            ChannelResolution::Unavailable,
            ChannelResolution::Constant))
  {
    return false;
  }
  switch (resolve->channels[channel]) {
    case ChannelResolution::Baked:
      return true;
    case ChannelResolution::Image:
      return !live_image_usable(resolve->images[channel], channel == PAINT_MATERIAL_CHANNEL_ALPHA);
    default:
      return false;
  }
}

bool BKE_paint_layers_material_row_needs_render(const Material &ma, const MaterialPaintLayer &row)
{
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (BKE_paint_layers_material_channel_needs_render(ma, row, channel)) {
      return true;
    }
  }
  return false;
}

bool paint_layer_material_row_needs_map(const Material &ma, const MaterialPaintLayer &row)
{
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (BKE_paint_layers_material_channel_needs_render(ma, row, channel) &&
        !paint_layer_material_source_group_channel(ma, row, channel))
    {
      return true;
    }
  }
  return false;
}

uint64_t paint_layer_material_live_signature(const Material &ma, const MaterialPaintLayer &row)
{
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&](const uint64_t value) { h = h * 1000003u ^ value; };
  if (BKE_paint_layers_material_row_is_canvas(row)) {
    /* A canvas row lays only its maps: its group has no source shape to follow, so the signature
     * names the scope and the reference alone. The subtree's content moves the row's source hash
     * instead, which re-renders the maps; the graph reads them through the same images, so a
     * rebuild would wire the same nodes back in.
     *
     * Why the content hash of the reference is not folded in and this is still complete: a source
     * edit fans out over every layered material in #bmain
     * (#BKE_paint_layers_source_material_consumers, called from the editor's material update), the
     * debounce of the ones that follow automatically is armed, and their tick re-renders the
     * canvas rows from #bmain -- whether or not any evaluated copy of the consumer is alive. A
     * consumer nobody evaluates gets its maps made fresh the moment something asks for one, since
     * the render path never consults a depsgraph. No shape move can ride along on a content move
     * either: the channel set a canvas row's group reads is the same whatever the source
     * supplies (every channel but the alpha renders), and the maps it reads are the same
     * datablocks re-filled in place, so the group the generator builds is the group it had. */
    mix(0x5100u + uint64_t(row.material_scope));
    mix(UUID(row.material_row_marker).hash());
    if (row.bake != nullptr && row.bake->coverage != nullptr) {
      /* The coverage map is read through a node the keyed map sync does not cover (the sync writes
       * the colour maps' pointers, not the coverage one). A coverage map replaced by a render -- a
       * new canvas shape makes a new datablock -- is therefore a rebuild, not a pointer write, or
       * the group would keep reading the map the row no longer holds. */
      mix(row.bake->coverage->id.session_uid);
    }
    return h;
  }
  if (row.role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    /* Two channels of the same kind of source (both plain values, say) differ in nothing else, but
     * the group inputs are named by the channel, so the generated graph does. */
    mix(0x600u + uint64_t(paint_layer_mask_source_channel(row)));
  }
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    /* A mask correction reads one channel; the others are not its shape. */
    if (row.role == MA_PAINT_LAYER_ROLE_MASK_ITEM && channel != paint_layer_mask_source_channel(row))
    {
      continue;
    }
    const PaintLayerMaterialSource source = paint_layer_material_channel_source(ma, row, channel);
    mix(uint64_t(source.kind));
    if (source.image != nullptr) {
      mix(source.image->id.session_uid);
    }
    if (source.kind == PaintLayerMaterialSourceKind::LiveImage && source.node != nullptr) {
      if (const NodeTexImage *storage = static_cast<const NodeTexImage *>(source.node->storage)) {
        mix(uint64_t(storage->interpolation) | (uint64_t(storage->extension) << 8));
      }
    }
  }
  const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(ma, row);
  mix(uint64_t(coverage.kind));
  if (coverage.image != nullptr) {
    mix(coverage.image->id.session_uid);
  }
  if (coverage.kind == PaintLayerMaterialCoverageKind::LiveAlpha && coverage.node != nullptr) {
    if (const NodeTexImage *storage = static_cast<const NodeTexImage *>(coverage.node->storage)) {
      mix(uint64_t(storage->interpolation) | (uint64_t(storage->extension) << 8));
    }
  }
  return h;
}

}  // namespace blender
