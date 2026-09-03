/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Resolves what a #Material can supply to each PBR Paint channel.
 *
 * Deliberately free of GPU and of #Object: the answer depends only on the node tree, so the UI
 * can ask for it on every redraw without triggering a shader compile, and the paint path and the
 * UI can never disagree about which channels are usable.
 */

#include <array>
#include <cstdint>

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "DNA_scene_types.h"

namespace blender {

struct bNode;
struct bNodeSocket;
struct Image;
struct ImageUser;
struct Material;

/** What a channel gets from the source material. */
enum class ChannelResolution : int8_t {
  /** The material cannot supply this channel; strokes skip it entirely. */
  Unavailable = 0,
  /** The Principled input is unlinked: its default value is the whole answer, no bake needed. */
  Constant,
  /**
   * The Principled input is driven by a plain #ShaderNodeTexImage: the map already exists, so the
   * stroke samples its pixels directly. This is the common authoring case, and separating it from
   * #Baked is what lets a material with assigned textures paint without any GPU work.
   */
  Image,
  /** The Principled input is driven by a node graph and has to be baked. */
  Baked,
};

/**
 * Why a channel is unavailable. An enum rather than a string because the resolver runs on every
 * panel redraw: it must not allocate, and the UI needs a translatable message it composes itself.
 */
enum class ChannelUnavailableReason : int8_t {
  None = 0,
  NoMaterial,
  NoNodeTree,
  NoPrincipled,
  AmbiguousOutput,
  NoSocketForChannel,
  SocketMissingOnNode,
  NormalNotTangentSpace,
  NormalStrengthNotOne,
  /**
   * The Normal input has no source at all. A linked one that does not go through a Normal Map is
   * baked as the vector it produces, so this no longer covers that case.
   */
  NormalNotThroughNormalMap,
  GpuCompileFailed,
  /** The input is an Image Texture, but not one that can be sampled (no image, or a UDIM tile
   * set, which the stroke has no single buffer for). */
  ImageNotSampleable,
};

/**
 * The #ShaderNodeTexImage driving a channel resolved as #ChannelResolution::Image.
 *
 * Both pointers are into the node's own storage and are owned by the material; they stay valid
 * for as long as the node tree is not edited.
 */
struct ChannelSourceImage {
  Image *image = nullptr;
  /** The node's #NodeTexImage::iuser. Copy it before acquiring a buffer: acquisition writes to it
   * and the material is not the stroke's to mutate. */
  const ImageUser *iuser = nullptr;
};

/**
 * The Principled BSDF that drives \a ma's active Material Output, or null.
 *
 * Follows reroutes, node groups and muted nodes on the way from the output's Surface input, the
 * same way the render does, so that a graph which renders through a Principled also paints
 * through it.
 *
 * \param r_reason: set to the reason on failure, #ChannelUnavailableReason::None on success.
 * \param r_group_path: when given, receives the group instance nodes leading to the node, since
 *                      the Surface input is resolved the same way any other socket is and the
 *                      Principled may therefore live inside a nested group. Any path reported for
 *                      one of its own inputs is relative to the tree the node is in, so a caller
 *                      routing such a socket out to the root tree has to prepend this one.
 */
const bNode *BKE_paint_material_principled_find(const Material &ma,
                                                ChannelUnavailableReason &r_reason,
                                                Vector<const bNode *> *r_group_path = nullptr);

/**
 * Resolve the socket feeding \a socket, skipping reroutes, muted nodes and group wrappers. The
 * returned socket can belong to a nested group tree and is null when the input has no source.
 *
 * \param r_group_path: when given, receives the group instance nodes the walk descended through,
 *                      outermost first. A group definition can be instanced more than once, and
 *                      the returned socket lives in the definition, so it alone does not say which
 *                      instance actually feeds \a socket. Anything that has to act on the source
 *                      in place -- routing it out to a render pass, say -- needs this path, since
 *                      searching for one afterwards can only find an arbitrary instance. Cleared
 *                      first, and left empty when the source is in the same tree as \a socket.
 */
const bNodeSocket *BKE_paint_material_source_socket(
    const bNodeSocket &socket, Vector<const bNode *> *r_group_path = nullptr);

/**
 * What the source material supplies to every PBR Paint channel.
 *
 * Cheap enough to call on every panel redraw: it walks the node tree and allocates nothing.
 */
struct MaterialSourceResolve {
  std::array<ChannelResolution, PAINT_MATERIAL_CHANNEL_NUM> channels;
  std::array<ChannelUnavailableReason, PAINT_MATERIAL_CHANNEL_NUM> reasons;
  /**
   * Socket default value for channels resolved as #ChannelResolution::Constant. Scalar channels
   * use \a x; color channels use \a xyz. Meaningless for the other resolutions.
   */
  std::array<float4, PAINT_MATERIAL_CHANNEL_NUM> constants;
  /** Set for channels resolved as #ChannelResolution::Image; default-constructed otherwise. */
  std::array<ChannelSourceImage, PAINT_MATERIAL_CHANNEL_NUM> images;
};

/**
 * Resolve every channel against \a ma. A null \a ma resolves everything to
 * #ChannelResolution::Unavailable rather than failing, so callers never branch first.
 */
MaterialSourceResolve BKE_paint_material_source_resolve(const Material *ma);

}  // namespace blender
