/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_material_resolve.hh"

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "NOD_shader.h"

namespace blender {

/**
 * The socket feeding \a socket after transparent nodes are skipped, or null when nothing drives
 * it. Reroutes carry no meaning of their own, and a muted node behaves as its internal links, so
 * both are followed rather than treated as the source.
 *
 * \param r_group_path: appended with each group instance node descended through, outermost first.
 *                      Never cleared here, so the recursion accumulates the whole path.
 */
static const bNodeSocket *resolve_source_socket(const bNodeSocket &socket,
                                                Vector<const bNode *> *r_group_path = nullptr)
{
  const bNodeSocket *current = &socket;
  /* A malformed tree can in principle cycle; bound the walk rather than trust the data. */
  for (int step = 0; step < 64; step++) {
    if (current->directly_linked_links().is_empty()) {
      return nullptr;
    }
    const bNodeLink *link = current->directly_linked_links()[0];
    if (!link->is_available() || link->is_muted()) {
      return nullptr;
    }
    const bNode &from_node = *link->fromnode;
    if (from_node.is_reroute()) {
      current = static_cast<const bNodeSocket *>(from_node.inputs.first);
      continue;
    }
    if (from_node.is_muted()) {
      const bNodeLink *internal = nullptr;
      for (const bNodeLink &candidate : from_node.internal_links()) {
        if (candidate.tosock == link->fromsock) {
          internal = &candidate;
          break;
        }
      }
      if (internal == nullptr) {
        return nullptr;
      }
      current = internal->fromsock;
      continue;
    }
    if (from_node.is_group() && from_node.id != nullptr) {
      const bNodeTree *group_tree_ptr = id_cast<const bNodeTree *>(from_node.id);
      if (group_tree_ptr == nullptr) {
        return nullptr;
      }
      const bNodeTree &group_tree = *group_tree_ptr;
      group_tree.ensure_topology_cache();
      const bNode *group_output = group_tree.group_output_node();
      if (group_output == nullptr) {
        return nullptr;
      }
      const bNodeSocket *inner = bke::node_find_socket(
          *group_output, SOCK_IN, link->fromsock->identifier_ustr());
      if (inner == nullptr) {
        return nullptr;
      }
      /* Recorded before descending: the walk continues inside the group definition, which is
       * shared by every instance of it, so this node is the only record of which instance the
       * source was actually reached through. */
      if (r_group_path != nullptr) {
        r_group_path->append(&from_node);
      }
      return resolve_source_socket(*inner, r_group_path);
    }
    return link->fromsock;
  }
  return nullptr;
}

const bNodeSocket *BKE_paint_material_source_socket(const bNodeSocket &socket,
                                                    Vector<const bNode *> *r_group_path)
{
  if (r_group_path != nullptr) {
    r_group_path->clear();
  }
  return resolve_source_socket(socket, r_group_path);
}

const bNode *BKE_paint_material_principled_find(const Material &ma,
                                                ChannelUnavailableReason &r_reason,
                                                Vector<const bNode *> *r_group_path)
{
  if (r_group_path != nullptr) {
    r_group_path->clear();
  }
  if (ma.nodetree == nullptr) {
    r_reason = ChannelUnavailableReason::NoNodeTree;
    return nullptr;
  }
  const bNodeTree &ntree = *ma.nodetree;
  ntree.ensure_topology_cache();

  const bNode *output = ntreeShaderOutputNode(const_cast<bNodeTree *>(&ntree), SHD_OUTPUT_ALL);
  if (output == nullptr) {
    r_reason = ChannelUnavailableReason::AmbiguousOutput;
    return nullptr;
  }

  const bNodeSocket *surface = bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr);
  if (surface == nullptr) {
    r_reason = ChannelUnavailableReason::NoPrincipled;
    return nullptr;
  }

  const bNodeSocket *source = resolve_source_socket(*surface, r_group_path);
  if (source == nullptr || source->owner_node().type_legacy != SH_NODE_BSDF_PRINCIPLED) {
    /* A Mix/Add Shader has no single Principled to read: picking one of its branches would give
     * an arbitrary answer, so the whole material is reported as unusable instead. */
    r_reason = ChannelUnavailableReason::NoPrincipled;
    return nullptr;
  }

  r_reason = ChannelUnavailableReason::None;
  return &source->owner_node();
}

/** Fills \a r_resolve with \a reason for every channel. Used for whole-material failures. */
static void resolve_all_unavailable(MaterialSourceResolve &r_resolve,
                                    const ChannelUnavailableReason reason)
{
  r_resolve.channels.fill(ChannelResolution::Unavailable);
  r_resolve.reasons.fill(reason);
  r_resolve.constants.fill(float4(0.0f));
  r_resolve.images.fill(ChannelSourceImage{});
}

/**
 * Resolve \a source as a plain image map, if it is one.
 *
 * Only a #ShaderNodeTexImage counts: anything else is a graph that has to be evaluated. A tiled
 * (UDIM) image is rejected because a stroke samples one buffer, and which tile that would be is
 * not a property of the material.
 *
 * \return false when \a source is not an image map at all, leaving \a r_reason untouched so the
 *         caller can fall back to #ChannelResolution::Baked.
 */
static bool resolve_image_source(const bNodeSocket &source,
                                 ChannelSourceImage &r_image,
                                 ChannelUnavailableReason &r_reason)
{
  const bNode &node = source.owner_node();
  if (node.type_legacy != SH_NODE_TEX_IMAGE) {
    return false;
  }
  /* From here on the answer is "this is an image map", so every failure is reported rather than
   * handed back to the bake path: a broken Image Texture is not something baking would fix. */
  if (node.id == nullptr || GS(node.id->name) != ID_IM) {
    r_reason = ChannelUnavailableReason::ImageNotSampleable;
    return true;
  }
  const NodeTexImage *storage = static_cast<const NodeTexImage *>(node.storage);
  if (storage == nullptr) {
    r_reason = ChannelUnavailableReason::ImageNotSampleable;
    return true;
  }
  Image *image = id_cast<Image *>(node.id);
  if (image->source == IMA_SRC_TILED) {
    r_reason = ChannelUnavailableReason::ImageNotSampleable;
    return true;
  }
  r_image.image = image;
  r_image.iuser = &storage->iuser;
  r_reason = ChannelUnavailableReason::None;
  return true;
}

/** Reads \a socket's own value into \a r_constant, whatever its socket type. */
static void socket_default_to_constant(const bNodeSocket &socket, float4 &r_constant)
{
  switch (socket.type) {
    case SOCK_FLOAT: {
      const auto &value = *static_cast<const bNodeSocketValueFloat *>(socket.default_value);
      r_constant = float4(value.value, value.value, value.value, 1.0f);
      break;
    }
    case SOCK_RGBA: {
      const auto &value = *static_cast<const bNodeSocketValueRGBA *>(socket.default_value);
      r_constant = float4(value.value);
      break;
    }
    case SOCK_VECTOR: {
      const auto &value = *static_cast<const bNodeSocketValueVector *>(socket.default_value);
      r_constant = float4(value.value[0], value.value[1], value.value[2], 1.0f);
      break;
    }
    default:
      r_constant = float4(0.0f);
      break;
  }
}

/** The encoded RGB of an unperturbed tangent-space normal, i.e. `(0, 0, 1) * 0.5 + 0.5`. */
static constexpr float4 FLAT_NORMAL_CONSTANT = float4(0.5f, 0.5f, 1.0f, 1.0f);

/**
 * Resolve the Normal channel, which cannot follow the generic rule.
 *
 * PBR Paint writes tangent-space RGB, but the Principled's Normal input takes an already
 * transformed vector, from which the map cannot be recovered. So a #ShaderNodeNormalMap is read
 * one step earlier, at its own Color input, which is the encoded map itself. Every other source
 * is baked as the vector it produces and encoded during the bake: on the bake quad the tangent
 * basis is the identity, so the vector is already the map.
 */
static void resolve_normal_channel(const bNodeSocket &normal_socket,
                                   MaterialSourceResolve &r_resolve)
{
  const int channel = PAINT_MATERIAL_CHANNEL_NORMAL;
  r_resolve.channels[channel] = ChannelResolution::Unavailable;

  const bNodeSocket *source = resolve_source_socket(normal_socket);
  if (source == nullptr) {
    /* A material that says nothing about relief is not asking for flat pixels to be painted over
     * whatever normal map the target already has. Unavailable leaves the target alone. */
    r_resolve.reasons[channel] = ChannelUnavailableReason::NormalNotThroughNormalMap;
    return;
  }
  if (source->owner_node().type_legacy != SH_NODE_NORMAL_MAP) {
    /* A #ShaderNodeBump without a Height source has no relief to bake: its derivatives are zero
     * and it passes its Normal input through, so answer with the flat map instead of spending a
     * render on a uniform buffer. */
    if (source->owner_node().type_legacy == SH_NODE_BUMP) {
      const bNodeSocket *height = bke::node_find_socket(
          source->owner_node(), SOCK_IN, "Height"_ustr);
      if (height == nullptr || resolve_source_socket(*height) == nullptr) {
        r_resolve.channels[channel] = ChannelResolution::Constant;
        r_resolve.reasons[channel] = ChannelUnavailableReason::None;
        r_resolve.constants[channel] = FLAT_NORMAL_CONSTANT;
        return;
      }
    }
    r_resolve.channels[channel] = ChannelResolution::Baked;
    r_resolve.reasons[channel] = ChannelUnavailableReason::None;
    return;
  }

  const bNode &normal_map = source->owner_node();
  const auto &storage = *static_cast<const NodeShaderNormalMap *>(normal_map.storage);
  if (storage.space != SHD_SPACE_TANGENT) {
    r_resolve.reasons[channel] = ChannelUnavailableReason::NormalNotTangentSpace;
    return;
  }

  /* Strength is not a linear factor on the encoded RGB, and applying the node's own transform
   * would leave tangent space altogether, so a non-default Strength has no correct answer. */
  const bNodeSocket *strength = bke::node_find_socket(normal_map, SOCK_IN, "Strength"_ustr);
  if (strength == nullptr || resolve_source_socket(*strength) != nullptr ||
      static_cast<const bNodeSocketValueFloat *>(strength->default_value)->value != 1.0f)
  {
    r_resolve.reasons[channel] = ChannelUnavailableReason::NormalStrengthNotOne;
    return;
  }

  /* The encoded map is the Normal Map node's Color input, which is what a stroke has to sample:
   * the node's output is already transformed out of tangent space. */
  const bNodeSocket *color = bke::node_find_socket(normal_map, SOCK_IN, "Color"_ustr);
  if (color == nullptr) {
    r_resolve.reasons[channel] = ChannelUnavailableReason::SocketMissingOnNode;
    return;
  }
  const bNodeSocket *color_source = resolve_source_socket(*color);
  if (color_source == nullptr) {
    /* An unlinked Color is a constant, but a constant in the map's *encoded* space, which the
     * Constant path writes without the normal decode every other Normal source goes through.
     * Left as Baked rather than guessing at that conversion. */
    r_resolve.channels[channel] = ChannelResolution::Baked;
    r_resolve.reasons[channel] = ChannelUnavailableReason::None;
    return;
  }

  ChannelUnavailableReason image_reason = ChannelUnavailableReason::None;
  if (resolve_image_source(*color_source, r_resolve.images[channel], image_reason)) {
    r_resolve.channels[channel] = image_reason == ChannelUnavailableReason::None ?
                                      ChannelResolution::Image :
                                      ChannelResolution::Unavailable;
    r_resolve.reasons[channel] = image_reason;
    return;
  }

  r_resolve.channels[channel] = ChannelResolution::Baked;
  r_resolve.reasons[channel] = ChannelUnavailableReason::None;
}

MaterialSourceResolve BKE_paint_material_source_resolve(const Material *ma)
{
  MaterialSourceResolve resolve;
  if (ma == nullptr) {
    resolve_all_unavailable(resolve, ChannelUnavailableReason::NoMaterial);
    return resolve;
  }

  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  if (principled == nullptr) {
    resolve_all_unavailable(resolve, reason);
    return resolve;
  }

  resolve_all_unavailable(resolve, ChannelUnavailableReason::NoSocketForChannel);

  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.socket_name == nullptr) {
      /* Custom, Height and AO have no Principled input to read at all. */
      continue;
    }
    const bNodeSocket *socket = bke::node_find_socket(
        *principled, SOCK_IN, UString::from_ptr_noinline(info.socket_name));
    if (socket == nullptr) {
      /* An older Principled in the file may simply not have this input. */
      resolve.reasons[info.channel] = ChannelUnavailableReason::SocketMissingOnNode;
      continue;
    }
    if (info.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      resolve_normal_channel(*socket, resolve);
      continue;
    }
    const bNodeSocket *source = resolve_source_socket(*socket);
    if (source == nullptr) {
      resolve.channels[info.channel] = ChannelResolution::Constant;
      resolve.reasons[info.channel] = ChannelUnavailableReason::None;
      socket_default_to_constant(*socket, resolve.constants[info.channel]);
      continue;
    }
    ChannelUnavailableReason image_reason = ChannelUnavailableReason::None;
    if (resolve_image_source(*source, resolve.images[info.channel], image_reason)) {
      resolve.channels[info.channel] = image_reason == ChannelUnavailableReason::None ?
                                            ChannelResolution::Image :
                                            ChannelResolution::Unavailable;
      resolve.reasons[info.channel] = image_reason;
      continue;
    }
    resolve.channels[info.channel] = ChannelResolution::Baked;
    resolve.reasons[info.channel] = ChannelUnavailableReason::None;
  }

  return resolve;
}

}  // namespace blender
