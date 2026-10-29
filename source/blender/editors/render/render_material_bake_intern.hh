/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edrend
 *
 * The two halves of a material bake that the tests drive directly: what a #MaterialSourceResolve
 * becomes as bake requests, and what attaching those requests to a node tree does. The render
 * itself (`render_material_bake.cc`) is not declared here: it needs a GPU context, which a unit
 * test binary has none of, so the tests stop before it.
 *
 * Nothing outside `render_material_bake.cc` and its tests is meant to use either function.
 */

#include "BKE_paint_material_resolve.hh"

#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

struct Main;
struct Material;
struct bNode;
struct bNodeSocket;
struct bNodeTree;

namespace ed::material_bake {

/** One socket to bake, and the AOV it is delivered under. */
struct BakeSocketRequest {
  /** The socket to read. May live in a group nested anywhere inside the material's own tree. */
  const bNodeSocket *source = nullptr;
  /**
   * The group instance nodes leading to #source, outermost first; empty when it is in the root
   * tree. Must come from the same walk that found #source -- see #route_socket_to_root.
   */
  Vector<const bNode *> group_path;
  /** Unique within one bake: names the AOV, and the group output routed for it at every level. */
  char name[64] = "";
  /** Delivered as an #AOV_TYPE_COLOR pass rather than an #AOV_TYPE_VALUE one. */
  bool is_color = false;
  /** Encode the signed vector the socket produces into the [0, 1] range a normal map stores. */
  bool encode_vector = false;
};

/**
 * Describe every channel \a resolve marks as #ChannelResolution::Baked as a socket to bake.
 *
 * Reads \a bake_material but never edits it: attaching the AOVs is #bake_requests_attach's job, so
 * a caller that only wants to know whether the material can be baked at all can stop here.
 *
 * \param r_request_channels: the #eMaterialPaintChannel each request in \a r_requests stands for,
 *                            which is what maps the rendered buffers back onto channels.
 * \param include_height: also request the Height map, which is not a Principled input (see
 *                        #height_bake_source_socket); it is skipped, not refused, when the
 *                        material has none.
 * \return false when any of them has no resolvable source, which is reported rather than partially
 *         baked: a stroke that silently paints only some of the channels the material describes is
 *         harder to diagnose than one that reports the material as unusable.
 */
bool bake_channel_requests_build(const Material &bake_material,
                                 const MaterialSourceResolve &resolve,
                                 const bool include_image_resolution,
                                 Vector<BakeSocketRequest> &r_requests,
                                 Vector<int> &r_request_channels,
                                 const bool include_height);

/**
 * Attach an AOV output for every request, routing each source socket up to \a tree.
 *
 * Adds nodes and links to \a tree, which leaves its topology cache dirty; a caller that reads the
 * tree's topology afterwards has to bring it current itself.
 *
 * \return false when any of them could not be routed. Reported rather than partially baked: a
 *         stroke that silently paints only some of the requested sockets is harder to diagnose
 *         than one that reports the material as unusable.
 */
bool bake_requests_attach(Main &bmain,
                          bNodeTree &tree,
                          const Span<BakeSocketRequest> requests);

}  // namespace ed::material_bake
}  // namespace blender
