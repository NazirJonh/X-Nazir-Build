/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Write-target interface of the shape drawing tools.
 *
 * A #ShapeTargetBackend owns where a shape bake lands (image tiles, mesh PBR attributes, the
 * color attribute, PBVH pixels) and the undo/restore rules around it. The frontends and the
 * Vector session drive the same lifecycle without knowing the storage:
 *
 * - #begin: resolve the targets and create whatever is missing (channel maps, vertex attributes).
 *   Called once, before the first write and **outside any undo step**, so a bake never allocates
 *   as a side effect and the creation is not recorded twice (see #composite_targets_ensure_writable).
 * - #preview: write the current shape without pushing undo. The live drag / Vector preview uses
 *   it; the backend keeps enough state to undo the write through #cancel. Must be safe to call
 *   repeatedly with growing / changing shapes.
 * - #commit: write the shape and push exactly one undo step named \a undo_name.
 * - #cancel: restore whatever #preview wrote, without pushing undo (the operator was abandoned).
 *
 * A backend that has no live preview (the 3D sculpt canvases today) may implement #preview and
 * #cancel as `BLI_assert_unreachable()` with a TODO; the commit path is what runs.
 *
 * The \a shapes handed to #preview / #commit are already expanded by the SPACE's symmetry and the
 * \a style already carries its final per-channel values (brush fill, `style_channels_from_brush`);
 * a backend does not read the tool settings or expand symmetry itself.
 */

#pragma once

#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "paint_shape.hh"

namespace blender {

struct bContext;
struct Image;
struct ReportList;

}  // namespace blender

namespace blender::ed::sculpt_paint::shape {

class ShapeSpace;

class ShapeTargetBackend {
 public:
  virtual ~ShapeTargetBackend() = default;

  /** Resolve targets and create missing maps/attributes. \return false when nothing can be
   * written (the reason is reported on \a reports). */
  virtual bool begin(bContext &C, ReportList *reports) = 0;

  /** Write \a shapes without pushing undo; kept reversible through #cancel. \a C may be null for
   * a context-less refresh; a backend that needs context (notifiers, tags) skips it then.
   * \a reports is the current call's report list (may be null). */
  virtual bool preview(bContext *C,
                       ReportList *reports,
                       const Span<PaintShape> shapes,
                       const ShapeStyle &style) = 0;

  /** Write \a shapes and push one undo step named \a undo_name. \a C may be null only for the
   * context-less settle path; \a reports is the current call's report list (may be null). */
  virtual bool commit(bContext *C,
                      ReportList *reports,
                      const Span<PaintShape> shapes,
                      const ShapeStyle &style,
                      const char *undo_name) = 0;

  /** Restore the last #preview write, without undo. Context-less (called on teardown / undo). */
  virtual void cancel() = 0;

  /** Force the deferred redraw / shading tag of the last #preview even if the tag throttle would
   * skip it (the host calls it when an interaction settles). No-op for flat backends. */
  virtual void preview_force_update(bContext * /*C*/) {}

  /** Whether this canvas backend implements the live preview (#preview / #cancel). The Vector
   * session is offered only where it returns true; the others fall back to a one-shot bake. */
  virtual bool supports_preview() const
  {
    return false;
  }

  /**
   * Whether the write targets resolved at #begin are still valid. A backend that holds
   * long-lived target IDs (the image backend's channels) overrides this so a session can stop
   * before writing through a freed ID; a stateless backend is always alive.
   */
  virtual bool targets_alive() const
  {
    return true;
  }

  /**
   * Point the backend at a replacement space: the live session re-anchors its space to the
   * (possibly deformed) surface before a commit. The 3D backends read the space on every
   * preview / commit call and cache nothing from it, so the replacement -- done while both spaces
   * are alive, before the old one is destroyed -- is a plain pointer swap.
   */
  virtual void space_replace(const ShapeSpace & /*space*/) {}

  /**
   * The images this backend writes, for the Image Editor linkage of a live 3D session (an editor
   * showing one of them displays the session's contour / cage). Empty for a non-image backend.
   */
  virtual void target_images(Vector<const Image *> & /*r_images*/) const {}
};

}  // namespace blender::ed::sculpt_paint::shape
