/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Self-heal for the generated node graph of a layered material: an undo or redo decode can leave
 * the generated trees with dangling references (a group node without its tree, a stale group
 * interface, an image node without its image) while the graph still counts as up to date, and the
 * object then renders black. The check detects those defects and rebuilds the graph through the
 * same regeneration the layer stack edits use.
 */

namespace blender {

struct Main;
struct Material;

/**
 * Check the generated graph of the layered material \a ma and rebuild it when the check finds a
 * defect that the graph cannot recover from on its own:
 *
 * - a group node without a tree, or whose tree is not in #Main;
 * - the owner's stack tree not in #Main;
 * - a group node whose sockets no longer match its tree's interface;
 * - an Image Texture without an image, or with a dangling one, in the root tree or in a row group
 *   whose stack records still hold maps (including mesh-map slots and bake copies). An imageless
 *   Image Texture inside a .PL Source copy of the user's own graph can be intentional and never
 *   triggers.
 *
 * Informational defects (an invalid link, a stack-versus-graph mismatch, defects of the stack
 * records themselves) never trigger a rebuild.
 *
 * The rebuild drops the stored topology hashes of the material's trees first, so the regeneration
 * rebuilds every group from the description instead of keeping the broken state, and then requests
 * the same regeneration the stack edits make. It never creates an undo step and never re-enters
 * the undo code: at most one rebuild attempt per call. Materials without layers, linked and
 * override materials are skipped.
 *
 * Main thread only. After a rebuild the material is tagged for a shading update; raising a
 * notifier for the viewport is the caller's business.
 *
 * \return True when a rebuild was performed.
 */
bool BKE_paint_layers_graph_check_and_heal(Main &bmain, Material &ma);

}  // namespace blender
